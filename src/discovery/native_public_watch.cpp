#include "native_public_watch.hpp"

#include "../core/deferred_events.hpp"
#include "../core/wall_clock.hpp"
#include "native_discovery_cycle.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
class PublicWatch final : public DiscoveryBackend,
                          public std::enable_shared_from_this<PublicWatch> {
  public:
    PublicWatch(CallbackGate gate, DiscoveryHandlers handlers);
    void initialize(MemoryBudget budget, WatchOptions options);
    void close() override;
    [[nodiscard]] bool healthy() const override;
    [[nodiscard]] bool recovering() const override;
    [[nodiscard]] bool is_current(const Advertisement &value) override;
    [[nodiscard]] std::optional<PeerSnapshot> peer_snapshot(const Advertisement &value) override;
    [[nodiscard]] std::optional<PeerCollection> current_peers() override;
    void set_capability(std::shared_ptr<const Bytes> capability) {
        capability_ = std::move(capability);
    }
    void peer(Advertisement value, PeerEvent event) noexcept;
    void catchup(bool complete) noexcept;
    void status(DiscoveryStatus value) noexcept;
    void fail(WatchError error) noexcept;
    [[nodiscard]] DiscoveryCycleCallbacks callbacks();
    [[nodiscard]] DiscoveryWatchCallbacks watch_callbacks();
    std::unique_ptr<NativeDiscoveryCycle> cycle;

  private:
    void enqueue(std::size_t bytes, std::function<void()> callback) noexcept;
    CallbackGate gate_;
    std::unique_ptr<DeferredEvents> events_;
    DiscoveryHandlers handlers_;
    std::shared_ptr<const Bytes> capability_;
    std::uint64_t max_lifetime_ = 0;
    std::uint64_t clock_skew_ = 0;
    DiscoveryProtocol protocol_ = DiscoveryProtocol::FixedKeyV1;
    bool active_ = true;
    bool failed_ = false;
};

PublicWatch::PublicWatch(CallbackGate gate, DiscoveryHandlers handlers)
    : gate_(gate), handlers_(std::move(handlers)) {}

void PublicWatch::initialize(MemoryBudget budget, WatchOptions options) {
    max_lifetime_ = options.max_advertisement_lifetime;
    clock_skew_ = options.clock_skew;
    protocol_ = options.protocol;
    auto weak = weak_from_this();
    auto gate = gate_;
    Executor delivery = [weak, gate](auto task) {
        auto self = weak.lock();
        if (!self || !gate.post([self, task = std::move(task)]() mutable { task(); })) {
            throw std::logic_error("node callback gate closed during discovery");
        }
    };
    events_ = std::make_unique<DeferredEvents>(std::move(delivery), std::move(budget),
                                               options.max_pending_events,
                                               options.max_pending_events * sizeof(Advertisement));
}

void PublicWatch::close() {
    if (!active_) {
        return;
    }
    active_ = false;
    if (events_) {
        events_->close();
    }
    if (cycle) {
        cycle->close();
    }
}

bool PublicWatch::healthy() const {
    return active_ && !failed_ && cycle && cycle->watch_available();
}
bool PublicWatch::recovering() const {
    return active_ && !failed_ && cycle && cycle->watch_recovering();
}

bool PublicWatch::is_current(const Advertisement &value) {
    return healthy() && cycle->is_current(value);
}

std::optional<PeerSnapshot> PublicWatch::peer_snapshot(const Advertisement &value) {
    if (!is_current(value) || !capability_) {
        return std::nullopt;
    }
    return PeerSnapshot{value, capability_, max_lifetime_, clock_skew_, protocol_};
}

std::optional<PeerCollection> PublicWatch::current_peers() {
    if (!healthy() || !capability_) {
        return std::nullopt;
    }
    auto peers = cycle->current_peers();
    if (!peers) {
        return std::nullopt;
    }
    return PeerCollection{std::move(*peers), capability_, max_lifetime_, clock_skew_, protocol_};
}

void PublicWatch::enqueue(std::size_t bytes, std::function<void()> callback) noexcept {
    if (!active_ || failed_) {
        return;
    }
    try {
        if (events_->post(bytes, std::move(callback)) != DeferredEventResult::Accepted) {
            fail(WatchError::ResourceLimit);
        }
    } catch (...) {
        fail(WatchError::ResourceLimit);
    }
}

void PublicWatch::peer(Advertisement value, PeerEvent event) noexcept {
    try {
        auto weak = weak_from_this();
        enqueue(sizeof(Advertisement), [weak, value = std::move(value), event]() mutable {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                try {
                    self->handlers_.on_peer(std::move(value), event);
                } catch (...) {
                    self->fail(WatchError::Callback);
                    throw;
                }
            }
        });
    } catch (...) {
        fail(WatchError::ResourceLimit);
    }
}

void PublicWatch::catchup(bool complete) noexcept {
    try {
        auto weak = weak_from_this();
        enqueue(0, [weak, complete] {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                try {
                    self->handlers_.on_catchup(complete);
                } catch (...) {
                    self->fail(WatchError::Callback);
                    throw;
                }
            }
        });
    } catch (...) {
        fail(WatchError::ResourceLimit);
    }
}

void PublicWatch::status(DiscoveryStatus value) noexcept {
    if (!handlers_.on_status)
        return;
    try {
        auto weak = weak_from_this();
        enqueue(0, [weak, value] {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                try {
                    self->handlers_.on_status(value);
                } catch (...) {
                    self->fail(WatchError::Callback);
                    throw;
                }
            }
        });
    } catch (...) {
        fail(WatchError::ResourceLimit);
    }
}

void PublicWatch::fail(WatchError error) noexcept {
    if (!active_ || failed_) {
        return;
    }
    failed_ = true;
    try {
        (void)gate_.post([self = shared_from_this(), error] {
            if (self->active_) {
                self->events_->close();
                if (self->cycle) {
                    self->cycle->close();
                }
                if (self->active_) {
                    self->handlers_.on_error(error);
                }
            }
        });
    } catch (...) {
        std::cerr << "p2pdc: discovery error delivery failed; subscription is unhealthy\n";
    }
}

DiscoveryWatchCallbacks PublicWatch::watch_callbacks() {
    auto weak = weak_from_this();
    return {[weak](Advertisement value, AdvertisementEvent event) {
                if (auto self = weak.lock()) {
                    self->peer(std::move(value), event == AdvertisementEvent::Seen
                                                     ? PeerEvent::Seen
                                                     : PeerEvent::Updated);
                }
            },
            [weak](Advertisement value) {
                if (auto self = weak.lock()) {
                    self->peer(std::move(value), PeerEvent::Expired);
                }
            },
            [weak](DiscoveryWatchError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock()) {
                    auto result = error == DiscoveryWatchError::ResourceLimit ||
                                          detail == StoreError::ResourceLimit ||
                                          detail == StoreError::AllocationFailed
                                      ? WatchError::ResourceLimit
                                  : error == DiscoveryWatchError::ClockRegressed ? WatchError::Clock
                                  : error == DiscoveryWatchError::CryptoFailure
                                      ? WatchError::Crypto
                                      : WatchError::Backend;
                    self->fail(result);
                }
            },
            [weak](bool result) {
                if (auto self = weak.lock()) {
                    self->catchup(result);
                }
            },
            [weak](RecoveryState value) {
                if (auto self = weak.lock())
                    self->status(value == RecoveryState::Reconnecting
                                     ? DiscoveryStatus::Reconnecting
                                     : DiscoveryStatus::Ready);
            }};
}

DiscoveryCycleCallbacks PublicWatch::callbacks() {
    auto weak = weak_from_this();
    return {watch_callbacks(), {}, [weak](DiscoveryCycleError error) {
                if (auto self = weak.lock()) {
                    self->fail(error == DiscoveryCycleError::Scheduler ? WatchError::Scheduler
                                                                       : WatchError::Backend);
                }
            }};
}

} // namespace

void validate_watch(const Scheduler &scheduler, const WatchOptions &options,
                    const DiscoveryHandlers &handlers) {
    if (!scheduler || !handlers.on_peer || !handlers.on_catchup || !handlers.on_error ||
        !options.max_peers || options.max_peers > 1024 || !options.max_pending_events ||
        options.max_pending_events > 1024 || options.pulse_interval.count() < 10 ||
        !options.max_advertisement_lifetime || options.max_recovery_retries > 10 ||
        options.recovery_delay.count() < 10 || options.recovery_delay > std::chrono::minutes(1) ||
        (options.protocol != DiscoveryProtocol::FixedKeyV1 &&
         options.protocol != DiscoveryProtocol::RotatingKeysV2) ||
        (options.protocol == DiscoveryProtocol::RotatingKeysV2 &&
         (options.clock_skew > 420 ||
          options.max_advertisement_lifetime > 840 - 2 * options.clock_skew))) {
        throw std::invalid_argument("invalid discovery watch settings");
    }
}

DiscoveryBackendResult make_public_watch(RecordStore store, DiscoveryCapability capability,
                                         Scheduler scheduler, WatchOptions options,
                                         DiscoveryHandlers handlers, Executor executor,
                                         CallbackGate gate, MemoryBudget budget) {
    validate_watch(scheduler, options, handlers);
    auto state = std::make_shared<PublicWatch>(gate, std::move(handlers));
    state->initialize(budget, options);
    auto secret = std::make_shared<const Bytes>(capability.begin(), capability.end());
    state->set_capability(secret);
    const auto raw_records = std::clamp(options.max_peers * 4, std::size_t{64}, std::size_t{1024});
    const auto raw_bytes = raw_records * 1024;
    DiscoveryWatchLimits limits{{raw_bytes, raw_records, 1024},
                                options.max_peers,
                                options.max_advertisement_lifetime,
                                options.clock_skew,
                                options.protocol == DiscoveryProtocol::RotatingKeysV2,
                                RecoveryOptions{options.max_recovery_retries,
                                                options.recovery_delay, std::chrono::minutes(1)}};
    DiscoveryCycleConfig config{
        store, store, {}, secret, limits, {}, budget, options.pulse_interval, 1, forward_utc_seconds};
    std::weak_ptr<PublicWatch> weak = state;
    Executor cycle_executor = [weak, executor = std::move(executor)](auto task) {
        if (auto self = weak.lock()) {
            executor([self, task = std::move(task)]() mutable { task(); });
        }
    };
    auto created = NativeDiscoveryCycle::create(std::move(config), std::move(cycle_executor),
                                                std::move(scheduler), state->callbacks());
    if (auto *error = std::get_if<StoreError>(&created)) {
        return *error == StoreError::ResourceLimit || *error == StoreError::AllocationFailed
                   ? WatchError::ResourceLimit
                   : WatchError::Backend;
    }
    if (std::holds_alternative<CryptoError>(created)) {
        return WatchError::Crypto;
    }
    state->cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(created));
    (void)state->cycle->start();
    return std::static_pointer_cast<DiscoveryBackend>(state);
}
} // namespace p2pdc::detail
