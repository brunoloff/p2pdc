#include "fan_out_store.hpp"

#include "backend_records.hpp"
#include "fan_out_watch.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {

// Loop thread only, like every state below.
struct PutState {
    std::size_t total;
    std::function<void(bool)> completion;
    std::size_t failed = 0;
    bool done = false;
    void settle(bool success) {
        if (done || (!success && ++failed < total))
            return;
        done = true;
        completion(success);
    }
};

struct LookupState {
    std::size_t total;
    std::function<void(LookupResult)> completion;
    StoreLimits limits;
    bool by_content;
    std::shared_ptr<BackendLoop> loop;
    std::chrono::milliseconds grace_period;
    std::vector<StoreBatch> answers = {};
    std::optional<StoreError> error = {};
    std::size_t settled = 0;
    BackendLoop::TimerId grace = 0;
    bool done = false;

    void settle(LookupResult result, const std::shared_ptr<LookupState> &self) {
        if (done)
            return;
        ++settled;
        const bool answered = result.backend_success || !result.batch.values.empty();
        if (answered)
            answers.push_back(std::move(result.batch));
        else if (!error)
            error = result.batch.error.value_or(StoreError::NotRunning);
        if (settled == total)
            finish();
        else if (answered && grace == 0)
            grace = loop->post_after(grace_period, [self] { self->finish(); });
    }
    void finish() {
        if (std::exchange(done, true))
            return;
        loop->cancel(grace);
        LookupResult result;
        if (answers.empty()) {
            result.batch.error = error.value_or(StoreError::NotRunning);
            completion(std::move(result));
            return;
        }
        RecordCollector collector(limits, by_content);
        std::size_t dropped = 0;
        for (const auto &answer : answers) {
            dropped += answer.dropped;
            for (const auto &value : answer.values)
                collector.add(BackendRecord{value, {}});
        }
        result.batch = collector.take();
        result.batch.dropped += dropped;
        result.backend_success = true;
        completion(std::move(result));
    }
};

// Posts a store's result to the loop, where the operation's state lives.
template <class State, class Result>
auto on_loop(std::shared_ptr<BackendLoop> loop, std::shared_ptr<State> state) {
    return [loop = std::move(loop), state = std::move(state)](Result result) {
        loop->post([state, result = std::move(result)]() mutable {
            if constexpr (std::is_same_v<State, LookupState>)
                state->settle(std::move(result), state);
            else
                state->settle(result);
        });
    };
}

class FanOutStore final : public RecordStoreBackend {
  public:
    FanOutStore(std::shared_ptr<BackendLoop> loop, std::vector<RecordStore> stores,
                StoreLimits limits, FanOutTiming timing)
        : loop_(std::move(loop)), stores_(std::move(stores)), limits_(limits), timing_(timing) {}

    std::optional<StoreError> put(ByteView key, ByteView payload,
                                  std::function<void(bool)> completion) const override {
        if (!completion)
            return StoreError::InvalidCallback;
        auto state = std::make_shared<PutState>(PutState{stores_.size(), std::move(completion)});
        return start(state, [&](const RecordStore &store) {
            return store.put(key, payload, on_loop<PutState, bool>(loop_, state));
        });
    }

    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> completion,
                                     LookupDeduplication deduplication) const override {
        if (!completion)
            return StoreError::InvalidCallback;
        auto state = std::make_shared<LookupState>(LookupState{
            stores_.size(), std::move(completion), limits_,
            deduplication == LookupDeduplication::Content, loop_, timing_.lookup_grace});
        return start(state, [&](const RecordStore &store) {
            return store.lookup(key, on_loop<LookupState, LookupResult>(loop_, state),
                                deduplication);
        });
    }

    std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const override {
        if (!callback)
            return StoreError::InvalidCallback;
        if (!record_key(key))
            return StoreError::InvalidKey;
        return start_fan_out_watch(loop_, stores_, key, std::move(callback), deduplicate, timing_);
    }

  private:
    // Starts the operation on every store. Stores that refuse at once count as failed; when
    // all do, the first refusal is returned and the completion is never called.
    template <class State, class Start>
    std::optional<StoreError> start(const std::shared_ptr<State> &state, Start begin) const {
        std::optional<StoreError> first;
        std::size_t refused = 0;
        for (const auto &store : stores_) {
            if (auto error = begin(store)) {
                first = first.value_or(*error);
                ++refused;
            }
        }
        if (refused == stores_.size())
            return first;
        if (refused == 0)
            return std::nullopt;
        const bool posted = loop_->post([state, refused] {
            for (std::size_t index = 0; index < refused; ++index) {
                if constexpr (std::is_same_v<State, LookupState>)
                    state->settle(LookupResult{StoreBatch{{}, StoreError::NotRunning}}, state);
                else
                    state->settle(false);
            }
        });
        return posted ? std::nullopt : std::optional(StoreError::NotRunning);
    }

    std::shared_ptr<BackendLoop> loop_;
    std::vector<RecordStore> stores_;
    StoreLimits limits_;
    FanOutTiming timing_;
};

} // namespace

RecordStore make_fan_out_store(std::shared_ptr<BackendLoop> loop, std::vector<RecordStore> stores,
                               StoreLimits limits, FanOutTiming timing) {
    if (!loop || stores.size() < 2)
        throw std::invalid_argument("fan-out needs a loop and two or more stores");
    return RecordStore(
        std::make_shared<FanOutStore>(std::move(loop), std::move(stores), limits, timing));
}

} // namespace p2pdc::detail
