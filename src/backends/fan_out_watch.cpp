#include "fan_out_watch.hpp"

#include "backend_records.hpp"
#include "watch_delivery.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <utility>

namespace p2pdc::detail {
namespace {

struct Member {
    RecordStore store;
    std::optional<RecordWatch> watch;
    std::shared_future<std::size_t> registration;
    std::chrono::milliseconds backoff;
    BackendLoop::TimerId retry = 0;
    std::uint64_t generation = 0;
    bool ready = false;
};

// Loop thread only, apart from the delivery.
class FanOutWatchState : public std::enable_shared_from_this<FanOutWatchState> {
  public:
    FanOutWatchState(std::shared_ptr<BackendLoop> loop, const std::vector<RecordStore> &stores,
                     RecordKey key, std::shared_ptr<WatchDelivery> delivery, bool deduplicate,
                     FanOutTiming timing)
        : loop_(std::move(loop)), key_(key), delivery_(std::move(delivery)),
          deduplicate_(deduplicate), timing_(timing) {
        for (const auto &store : stores)
            members_.push_back(Member{store, std::nullopt, {}, timing.rewatch});
    }
    void begin() {
        for (std::size_t index = 0; index < members_.size(); ++index)
            start(index);
    }
    void stop();

  private:
    void start(std::size_t index);
    void on_batch(std::size_t index, std::uint64_t generation, StoreBatch batch);
    void poll();
    void failed(std::size_t index, StoreError error);
    template <class Step> auto later(Step step) {
        return [weak = weak_from_this(), step] {
            if (auto self = weak.lock(); self && !self->stopped_)
                step(*self);
        };
    }

    std::shared_ptr<BackendLoop> loop_;
    RecordKey key_;
    std::shared_ptr<WatchDelivery> delivery_;
    bool deduplicate_;
    FanOutTiming timing_;
    std::vector<Member> members_;
    std::set<std::size_t> failures_;
    BackendLoop::TimerId poll_ = 0;
    bool ever_ready_ = false;
    bool stopped_ = false;
};

void FanOutWatchState::start(std::size_t index) {
    auto &member = members_[index];
    member.retry = 0;
    const auto generation = ++member.generation;
    auto callback = [loop = loop_, weak = weak_from_this(), index, generation](StoreBatch batch) {
        loop->post([weak, index, generation, batch = std::move(batch)]() mutable {
            if (auto self = weak.lock(); self && !self->stopped_)
                self->on_batch(index, generation, std::move(batch));
        });
    };
    auto result = member.store.watch(key_, std::move(callback), deduplicate_);
    if (auto *error = std::get_if<StoreError>(&result)) {
        failed(index, *error);
        return;
    }
    member.watch.emplace(std::get<RecordWatch>(std::move(result)));
    member.registration = member.watch->registration();
    if (poll_ == 0)
        poll();
}

void FanOutWatchState::on_batch(std::size_t index, std::uint64_t generation, StoreBatch batch) {
    if (generation != members_[index].generation)
        return;
    const auto error = std::exchange(batch.error, std::nullopt);
    if (!batch.values.empty() || batch.dropped || !error)
        delivery_->deliver(std::move(batch));
    if (error)
        failed(index, *error);
}

// Checks pending registrations: OpenDHT reports them through a future only.
void FanOutWatchState::poll() {
    poll_ = 0;
    bool pending = false;
    for (std::size_t index = 0; index < members_.size(); ++index) {
        auto &member = members_[index];
        if (!member.watch || member.ready)
            continue;
        if (member.registration.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            pending = true;
            continue;
        }
        if (member.registration.get() == 0) {
            failed(index, StoreError::NotRunning);
            if (stopped_)
                return;
            continue;
        }
        member.ready = true;
        member.backoff = timing_.rewatch;
        ever_ready_ = true;
        failures_.erase(index);
        delivery_->report(true);
    }
    if (pending)
        poll_ =
            loop_->post_after(timing_.registration_poll, later([](auto &self) { self.poll(); }));
}

void FanOutWatchState::failed(std::size_t index, StoreError error) {
    auto &member = members_[index];
    ++member.generation;
    member.ready = false;
    if (auto watch = std::exchange(member.watch, std::nullopt))
        watch->cancel();
    failures_.insert(index);
    if (!ever_ready_ && failures_.size() == members_.size()) {
        stop();
        delivery_->report(false);
        delivery_->deliver(StoreBatch{{}, error, false, 0});
        return;
    }
    member.retry =
        loop_->post_after(member.backoff, later([index](auto &self) { self.start(index); }));
    member.backoff = std::min(member.backoff * 2, timing_.rewatch_cap);
}

void FanOutWatchState::stop() {
    stopped_ = true;
    loop_->cancel(poll_);
    for (auto &member : members_) {
        loop_->cancel(member.retry);
        if (auto watch = std::exchange(member.watch, std::nullopt))
            watch->cancel();
    }
    delivery_->report(false);
}

class FanOutWatch final : public RecordWatchBackend {
  public:
    FanOutWatch(std::shared_ptr<BackendLoop> loop, std::shared_ptr<WatchDelivery> delivery,
                std::shared_ptr<FanOutWatchState> state)
        : loop_(std::move(loop)), delivery_(std::move(delivery)), state_(std::move(state)) {}
    ~FanOutWatch() override {
        delivery_->abandon();
        stop();
    }
    void cancel() override {
        delivery_->cancel();
        stop();
    }
    std::shared_future<std::size_t> registration() const override {
        return delivery_->registration();
    }

  private:
    // On the loop, where the state lives; inline once the loop has stopped and runs nothing.
    void stop() {
        if (!loop_->post([state = state_] { state->stop(); }))
            state_->stop();
    }
    std::shared_ptr<BackendLoop> loop_;
    std::shared_ptr<WatchDelivery> delivery_;
    std::shared_ptr<FanOutWatchState> state_;
};

} // namespace

std::variant<RecordWatch, StoreError> start_fan_out_watch(std::shared_ptr<BackendLoop> loop,
                                                          const std::vector<RecordStore> &stores,
                                                          ByteView key,
                                                          std::function<void(StoreBatch)> callback,
                                                          bool deduplicate, FanOutTiming timing) {
    const auto address = record_key(key);
    if (!address)
        return StoreError::InvalidKey;
    auto delivery = std::make_shared<WatchDelivery>(std::move(callback));
    auto state =
        std::make_shared<FanOutWatchState>(loop, stores, *address, delivery, deduplicate, timing);
    if (!loop->post([state] { state->begin(); }))
        return StoreError::NotRunning;
    return RecordWatch(
        std::make_unique<FanOutWatch>(std::move(loop), std::move(delivery), std::move(state)));
}

} // namespace p2pdc::detail
