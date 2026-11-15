#include "../src/browser/browser_mailbox_attempt.hpp"
#include "../src/browser/browser_record_store.hpp"
#include "p2pdc/runtime.h"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class ReadyWatch final : public RecordWatchBackend {
  public:
    ReadyWatch() : future_(ready_.get_future().share()) { ready_.set_value(1); }
    void cancel() override {}
    std::shared_future<std::size_t> registration() const override { return future_; }

  private:
    std::promise<std::size_t> ready_;
    std::shared_future<std::size_t> future_;
};
class ReadyStore final : public RecordStoreBackend {
  public:
    std::optional<StoreError> put(ByteView, ByteView,
                                  std::function<void(bool)> done) const override {
        done(true);
        return std::nullopt;
    }
    std::optional<StoreError> lookup(ByteView, std::function<void(LookupResult)> done,
                                     LookupDeduplication) const override {
        done(LookupResult{{}, true});
        return std::nullopt;
    }
    std::variant<RecordWatch, StoreError> watch(ByteView, std::function<void(StoreBatch)>,
                                                bool) const override {
        return RecordWatch(std::make_unique<ReadyWatch>());
    }
};
class FaultStore final : public RecordStoreBackend {
  public:
    explicit FaultStore(RecordStore inner) : inner_(std::move(inner)) {}
    std::optional<StoreError> put(ByteView key, ByteView value,
                                  std::function<void(bool)> done) const override {
        if (!first_)
            first_ = Bytes(value.begin(), value.end());
        return inner_.put(key, value, std::move(done));
    }
    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> done,
                                     LookupDeduplication deduplicate) const override {
        return inner_.lookup(key, std::move(done), deduplicate);
    }
    std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const override {
        watch_ = callback;
        return inner_.watch(key, std::move(callback), deduplicate);
    }
    void fail(StoreError error) const { watch_(StoreBatch{{}, error, false}); }
    std::optional<Bytes> first() const { return first_; }

  private:
    RecordStore inner_;
    mutable std::function<void(StoreBatch)> watch_;
    mutable std::optional<Bytes> first_;
};
std::uint64_t now_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("invalid test clock");
    return static_cast<std::uint64_t>(value);
}
struct Probe : std::enable_shared_from_this<Probe> {
    Probe(Runtime &runtime, std::function<void(bool)> done)
        : runtime(runtime), done(std::move(done)) {}
    Runtime &runtime;
    std::function<void(bool)> done;
    std::shared_ptr<Probe> keepalive;
    std::array<std::shared_ptr<Identity>, 2> identity;
    std::array<std::shared_ptr<RecipientKeyPair>, 2> boxes;
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{12});
    std::array<std::unique_ptr<BrowserMailboxAttempt>, 2> peers;
    std::unique_ptr<BrowserMailboxAttempt> offline;
    std::unique_ptr<BrowserMailboxAttempt> invalid_offer;
    std::array<std::shared_ptr<FaultStore>, 2> faults;
    std::array<bool, 2> started{}, ready{}, received{};
    bool offline_failed = false, unavailable = false, sent = false, finished = false;
    bool terminal_requested = false, terminal_failed = false, offer_rejected = false;
    CancelTimer deadline;
    BrowserMailboxConfig config(unsigned index, bool no_network = false) const;
    BrowserMailboxCallbacks handlers(unsigned index);
    bool setup();
    bool reject_accepted_offer();
    bool accepts_offer();
    void start();
    void poll();
    void on_ready(unsigned index, std::uint32_t limit);
    void signaling_lost(unsigned index);
    void failed(unsigned index, AttemptFailure reason);
    void transmit();
    void message(unsigned index, Bytes value);
    void check();
    void finish(bool success);
    void cancel();
};
BrowserMailboxConfig Probe::config(unsigned index, bool no_network) const {
    const auto other = 1U - index;
    AsyncSignalingBinding binding{
        {identity[index]->public_key(), {static_cast<std::byte>(index + 1)}},
        {identity[other]->public_key(), {static_cast<std::byte>(other + 1)}},
        boxes[index]->public_key(),
        boxes[other]->public_key(),
        AttemptId{std::byte{8}},
        index ? HandshakeRole::Answerer : HandshakeRole::Offerer};
    BrowserSecureConfig secure{binding, identity[index], boxes[index], capability,
                               {},      32768,           now_seconds};
    auto store = !no_network && faults[index]
                     ? RecordStore(faults[index])
                     : make_browser_record_store(no_network ? "http://127.0.0.1:1"
                                                            : "http://127.0.0.1:49412",
                                                 {32768, 128, 1048576}, no_network ? 1000 : 10000);
    return {store,
            {static_cast<std::byte>(index + 37)},
            {static_cast<std::byte>(other + 37)},
            std::move(secure),
            MemoryBudget(4194304),
            {}};
}
BrowserMailboxCallbacks Probe::handlers(unsigned index) {
    auto weak = weak_from_this();
    return {[weak, index](std::uint32_t value) {
                if (auto self = weak.lock())
                    self->on_ready(index, value);
            },
            [weak, index](Bytes value) {
                if (auto self = weak.lock())
                    self->message(index, std::move(value));
            },
            [] {},
            [](RouteInfo) {},
            [](BrowserTransportState) {},
            [weak, index] {
                if (auto self = weak.lock())
                    self->signaling_lost(index);
            },
            [weak, index](AttemptFailure reason) {
                if (auto self = weak.lock())
                    self->failed(index, reason);
            }};
}
bool Probe::setup() {
    for (auto &fault : faults)
        fault = std::make_shared<FaultStore>(
            make_browser_record_store("http://127.0.0.1:49412", {32768, 128, 1048576}));
    auto invalid = config(0);
    invalid.accepted_offer = Bytes{std::byte{1}};
    bool rejected = false;
    try {
        (void)BrowserMailboxAttempt::create(std::move(invalid), runtime.executor(), handlers(0));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected)
        return false;
    if (!reject_accepted_offer())
        return false;
    auto weak = weak_from_this();
    auto offline_handlers = handlers(1);
    offline_handlers.on_failure = [weak](AttemptFailure reason) {
        if (auto self = weak.lock()) {
            self->offline_failed = reason == AttemptFailure::Signaling;
            self->check();
        }
    };
    auto failed = BrowserMailboxAttempt::create(config(1, true), runtime.executor(),
                                                std::move(offline_handlers));
    if (!std::holds_alternative<std::unique_ptr<BrowserMailboxAttempt>>(failed))
        return false;
    offline = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(failed));
    for (unsigned index = 0; index < 2; ++index) {
        auto created =
            BrowserMailboxAttempt::create(config(index), runtime.executor(), handlers(index));
        if (!std::holds_alternative<std::unique_ptr<BrowserMailboxAttempt>>(created))
            return false;
        peers[index] = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(created));
    }
    return true;
}
bool Probe::reject_accepted_offer() {
    auto value = config(1);
    value.store = RecordStore(std::make_shared<ReadyStore>());
    value.accepted_offer = Bytes{std::byte{1}};
    auto weak = weak_from_this();
    auto callbacks = handlers(1);
    callbacks.on_failure = [weak](AttemptFailure reason) {
        if (auto self = weak.lock()) {
            self->offer_rejected = reason == AttemptFailure::Signaling;
            self->check();
        }
    };
    auto created =
        BrowserMailboxAttempt::create(std::move(value), runtime.executor(), std::move(callbacks));
    if (!std::holds_alternative<std::unique_ptr<BrowserMailboxAttempt>>(created))
        return false;
    invalid_offer = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(created));
    return invalid_offer->poll_start() == BrowserMailboxStart::Failed;
}
bool Probe::accepts_offer() {
    auto wire = faults[0]->first();
    if (!wire)
        return false;
    auto value = config(1);
    value.store = RecordStore(std::make_shared<ReadyStore>());
    value.accepted_offer = std::move(*wire);
    auto created = BrowserMailboxAttempt::create(std::move(value), runtime.executor(), handlers(1));
    if (!std::holds_alternative<std::unique_ptr<BrowserMailboxAttempt>>(created))
        return false;
    auto attempt = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(created));
    if (attempt->poll_start() != BrowserMailboxStart::Started ||
        attempt->poll_start() != BrowserMailboxStart::AlreadyStarted)
        return false;
    attempt->close();
    return attempt->poll_start() == BrowserMailboxStart::Closed;
}
void Probe::start() {
    keepalive = shared_from_this();
    for (unsigned i = 0; i < 2; ++i) {
        identity[i] = std::make_shared<Identity>(
            std::get<Identity>(Identity::import_seed(Bytes(32, static_cast<std::byte>(i + 11)))));
        boxes[i] = std::make_shared<RecipientKeyPair>(std::get<RecipientKeyPair>(
            RecipientKeyPair::import_secret_key(Bytes(32, static_cast<std::byte>(i + 13)))));
    }
    if (!setup()) {
        finish(false);
        return;
    }
    auto weak = weak_from_this();
    deadline = runtime.scheduler()(10s, [weak] {
        if (auto self = weak.lock())
            self->finish(false);
    });
    poll();
}
void Probe::poll() {
    if (finished)
        return;
    if (!offline_failed) {
        const auto status = offline->poll_start();
        if (status != BrowserMailboxStart::Pending && status != BrowserMailboxStart::Failed) {
            finish(false);
            return;
        }
    }
    for (unsigned index = 0; index < 2; ++index) {
        if (started[index]) {
            peers[index]->poll_health();
            continue;
        }
        const auto result = peers[index]->poll_start();
        if (result == BrowserMailboxStart::Failed || result == BrowserMailboxStart::Closed) {
            finish(false);
            return;
        }
        if (result == BrowserMailboxStart::Started)
            started[index] = true;
    }
    auto weak = weak_from_this();
    (void)runtime.scheduler()(20ms, [weak] {
        if (auto self = weak.lock())
            self->poll();
    });
}
void Probe::on_ready(unsigned index, std::uint32_t limit) {
    if (limit != 32768 || peers[index]->max_message_size() != limit) {
        finish(false);
        return;
    }
    ready[index] = true;
    if (ready[0] && ready[1] && !sent) {
        sent = true;
        auto weak = weak_from_this();
        runtime.executor()([weak] {
            if (auto self = weak.lock())
                self->faults[0]->fail(StoreError::NotRunning);
        });
    }
}
void Probe::failed(unsigned index, AttemptFailure reason) {
    if (index != 1 || !terminal_requested || reason != AttemptFailure::Resource ||
        peers[index]->ready()) {
        finish(false);
        return;
    }
    terminal_failed = true;
    check();
}
void Probe::signaling_lost(unsigned index) {
    if (index != 0 || !peers[index]->signaling_unavailable() || !peers[index]->ready() ||
        peers[index]->poll_start() != BrowserMailboxStart::AlreadyStarted) {
        finish(false);
        return;
    }
    unavailable = true;
    auto weak = weak_from_this();
    runtime.executor()([weak] {
        if (auto self = weak.lock())
            self->transmit();
    });
}
void Probe::transmit() {
    if (peers[0]->send(Bytes(16000, std::byte{255})) != ChannelSend::Accepted ||
        peers[1]->send(Bytes{std::byte{0}, std::byte{42}}) != ChannelSend::Accepted)
        finish(false);
}
void Probe::message(unsigned index, Bytes value) {
    const Bytes expected =
        index ? Bytes(16000, std::byte{255}) : Bytes{std::byte{0}, std::byte{42}};
    if (value != expected) {
        finish(false);
        return;
    }
    received[index] = true;
    check();
}
void Probe::check() {
    if (!offline_failed || !offer_rejected || !unavailable || !ready[0] || !ready[1] ||
        !received[0] || !received[1])
        return;
    if (!terminal_requested) {
        terminal_requested = true;
        auto weak = weak_from_this();
        runtime.executor()([weak] {
            if (auto self = weak.lock())
                self->faults[1]->fail(StoreError::ResourceLimit);
        });
        return;
    }
    if (terminal_failed) {
        auto weak = weak_from_this();
        runtime.executor()([weak] {
            if (auto self = weak.lock())
                self->finish(self->accepts_offer());
        });
    }
}
void Probe::finish(bool success) {
    if (finished)
        return;
    if (!success)
        std::cerr << "mailbox test start=" << started[0] << started[1] << " ready=" << ready[0]
                  << ready[1] << " received=" << received[0] << received[1]
                  << " offline=" << offline_failed << '\n';
    finished = true;
    if (deadline)
        deadline();
    if (offline)
        offline->close();
    if (invalid_offer)
        invalid_offer->close();
    for (auto &peer : peers)
        if (peer)
            peer->close();
    done(success);
    keepalive.reset();
}
void Probe::cancel() {
    if (finished)
        return;
    finished = true;
    if (deadline)
        deadline();
    if (offline)
        offline->close();
    if (invalid_offer)
        invalid_offer->close();
    for (auto &peer : peers)
        if (peer)
            peer->close();
    keepalive.reset();
}
} // namespace

std::function<void()> run_browser_mailbox_attempt_smoke(Runtime &runtime,
                                                        std::function<void(bool)> done) {
    auto probe = std::make_shared<Probe>(runtime, std::move(done));
    std::weak_ptr<Probe> weak = probe;
    probe->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
