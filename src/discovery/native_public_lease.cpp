#include "native_public_watch.hpp"

#include "../core/deferred_events.hpp"
#include "native_discovery_cycle.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
class PublicLease final : public DiscoveryBackend,
                          public std::enable_shared_from_this<PublicLease> {
  public:
    PublicLease(CallbackGate gate, AnnouncementEndpoint endpoint, AnnounceHandlers handlers)
        : gate_(gate), endpoint_(std::move(endpoint)), handlers_(std::move(handlers)) {}
    void initialize(MemoryBudget budget, AnnounceOptions options);
    void close() override;
    [[nodiscard]] bool healthy() const override;
    [[nodiscard]] std::optional<LeaseSnapshot> lease_snapshot() override;
    void set_capability(std::shared_ptr<const Bytes> capability) {
        capability_ = std::move(capability);
    }
    void published() noexcept;
    void retrying() noexcept;
    void fail(AnnouncementError error) noexcept;
    [[nodiscard]] DiscoveryCycleCallbacks callbacks();
    std::unique_ptr<NativeDiscoveryCycle> cycle;

  private:
    void enqueue(std::function<void()> event) noexcept;
    void stop_cycle();
    CallbackGate gate_;
    AnnouncementEndpoint endpoint_;
    std::shared_ptr<const Bytes> capability_;
    AnnounceHandlers handlers_;
    std::unique_ptr<DeferredEvents> events_;
    bool active_ = true;
    bool failed_ = false;
    DiscoveryProtocol protocol_ = DiscoveryProtocol::FixedKeyV1;
};

void PublicLease::initialize(MemoryBudget budget, AnnounceOptions options) {
    protocol_ = options.protocol;
    auto weak = weak_from_this();
    auto gate = gate_;
    Executor delivery = [weak, gate](auto task) {
        auto self = weak.lock();
        if (!self || !gate.post([self, task = std::move(task)]() mutable { task(); })) {
            throw std::logic_error("node callback gate closed during announcement");
        }
    };
    events_ = std::make_unique<DeferredEvents>(std::move(delivery), std::move(budget),
                                               options.max_pending_events, 0);
}

void PublicLease::close() {
    if (!active_) {
        return;
    }
    active_ = false;
    if (events_) {
        events_->close();
    }
    stop_cycle();
}

void PublicLease::stop_cycle() {
    if (cycle) {
        cycle->close();
        cycle.reset();
    }
    endpoint_.identity.reset();
    capability_.reset();
}

bool PublicLease::healthy() const { return active_ && !failed_ && cycle && cycle->is_open(); }

std::optional<LeaseSnapshot> PublicLease::lease_snapshot() {
    if (!healthy() || !endpoint_.identity || !capability_) {
        return std::nullopt;
    }
    auto fields = cycle->published_fields();
    if (!fields) {
        return std::nullopt;
    }
    return LeaseSnapshot{AnnouncementEndpoint{endpoint_.identity, *fields}, capability_, protocol_};
}

void PublicLease::enqueue(std::function<void()> event) noexcept {
    if (!active_ || failed_) {
        return;
    }
    try {
        if (events_->post(0, std::move(event)) != DeferredEventResult::Accepted) {
            fail(AnnouncementError::ResourceLimit);
        }
    } catch (...) {
        fail(AnnouncementError::ResourceLimit);
    }
}

void PublicLease::published() noexcept {
    try {
        enqueue([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                try {
                    self->handlers_.on_published();
                } catch (...) {
                    self->fail(AnnouncementError::Callback);
                    throw;
                }
            }
        });
    } catch (...) {
        fail(AnnouncementError::ResourceLimit);
    }
}

void PublicLease::retrying() noexcept {
    try {
        enqueue([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                try {
                    self->handlers_.on_error(AnnouncementError::Retrying);
                } catch (...) {
                    self->fail(AnnouncementError::Callback);
                    throw;
                }
            }
        });
    } catch (...) {
        fail(AnnouncementError::ResourceLimit);
    }
}

void PublicLease::fail(AnnouncementError error) noexcept {
    if (!active_ || failed_) {
        return;
    }
    failed_ = true;
    try {
        (void)gate_.post([self = shared_from_this(), error] {
            if (self->active_) {
                self->events_->close();
                self->stop_cycle();
                if (self->active_) {
                    self->handlers_.on_error(error);
                }
            }
        });
    } catch (...) {
        std::cerr << "p2pdc: announcement error delivery failed; lease is unhealthy\n";
    }
}

AnnouncementError lease_error(AdvertisementLeaseFailure failure) {
    switch (failure) {
    case AdvertisementLeaseFailure::PublicationLimit:
        return AnnouncementError::PublicationLimit;
    case AdvertisementLeaseFailure::ClockRegressed:
    case AdvertisementLeaseFailure::TimeOverflow:
        return AnnouncementError::Clock;
    case AdvertisementLeaseFailure::EncodingFailed:
        return AnnouncementError::Crypto;
    case AdvertisementLeaseFailure::AdmissionFailed:
        return AnnouncementError::ResourceLimit;
    default:
        return AnnouncementError::Backend;
    }
}

DiscoveryCycleCallbacks PublicLease::callbacks() {
    auto weak = weak_from_this();
    AdvertisementLeaseCallbacks lease{
        [weak](std::uint64_t) {
            if (auto self = weak.lock()) {
                self->published();
            }
        },
        [weak](AdvertisementLeaseError error) {
            if (auto self = weak.lock()) {
                if (auto *failure = std::get_if<AdvertisementLeaseFailure>(&error)) {
                    self->fail(lease_error(*failure));
                } else {
                    self->retrying();
                }
            }
        }};
    return {{}, std::move(lease), [weak](DiscoveryCycleError error) {
                if (auto self = weak.lock()) {
                    self->fail(error == DiscoveryCycleError::Scheduler
                                   ? AnnouncementError::Scheduler
                                   : AnnouncementError::Backend);
                }
            }};
}

std::uint64_t now_seconds() {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    if (seconds < 0) {
        throw std::runtime_error("system clock is before the Unix epoch");
    }
    return static_cast<std::uint64_t>(seconds);
}

} // namespace

void validate_announcement(const Scheduler &scheduler, const AnnounceOptions &options,
                           const AnnounceHandlers &handlers) {
    if (!scheduler || !handlers.on_published || !handlers.on_error ||
        options.pulse_interval.count() < 10 || !options.lifetime_seconds ||
        options.lifetime_seconds > 300 || !options.refresh_seconds ||
        options.refresh_seconds >= options.lifetime_seconds || !options.max_publications ||
        options.max_publications > 32 || !options.max_pending_events ||
        options.max_pending_events > 256 ||
        (options.protocol != DiscoveryProtocol::FixedKeyV1 &&
         options.protocol != DiscoveryProtocol::RotatingKeysV2)) {
        throw std::invalid_argument("invalid announcement settings");
    }
}

LeaseBackendResult make_public_lease(RecordStore store,
                                     AnnouncementEndpoint endpoint, DiscoveryCapability capability,
                                     Scheduler scheduler, AnnounceOptions options,
                                     AnnounceHandlers handlers, Executor executor,
                                     CallbackGate gate, MemoryBudget budget) {
    validate_announcement(scheduler, options, handlers);
    auto state = std::make_shared<PublicLease>(gate, endpoint, std::move(handlers));
    state->initialize(budget, options);
    auto secret = std::make_shared<const Bytes>(capability.begin(), capability.end());
    state->set_capability(secret);
    AdvertisementLeaseConfig lease{endpoint.fields,
                                   options.lifetime_seconds,
                                   options.refresh_seconds,
                                   300,
                                   30,
                                   options.max_publications,
                                   {65536, 1, 1024},
                                   options.protocol == DiscoveryProtocol::RotatingKeysV2};
    DiscoveryCycleConfig config{store, store,  endpoint.identity->signing, secret, {},
                                lease, budget, options.pulse_interval,     1,      now_seconds};
    std::weak_ptr<PublicLease> weak = state;
    Executor cycle_executor = [weak, executor = std::move(executor)](auto task) {
        if (auto self = weak.lock()) {
            executor([self, task = std::move(task)]() mutable { task(); });
        }
    };
    auto created = NativeDiscoveryCycle::create(std::move(config), std::move(cycle_executor),
                                                std::move(scheduler), state->callbacks());
    if (std::holds_alternative<StoreError>(created)) {
        return AnnouncementError::Backend;
    }
    if (std::holds_alternative<CryptoError>(created)) {
        return AnnouncementError::Crypto;
    }
    state->cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(created));
    (void)state->cycle->start();
    return std::static_pointer_cast<DiscoveryBackend>(state);
}
} // namespace p2pdc::detail
