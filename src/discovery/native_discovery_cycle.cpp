#include "native_discovery_cycle.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
struct NativeDiscoveryCycle::State : std::enable_shared_from_this<State> {
    TimerProducer timer;
    std::chrono::milliseconds period;
    std::size_t retry_delay;
    std::size_t retry_remaining = 0;
    DiscoveryCycleCallbacks callbacks;
    std::unique_ptr<NativeDiscoveryWatch> watch;
    std::unique_ptr<NativeAdvertisementLease> lease;
    bool active = true;
    bool started = false;
    bool catchup_started = false;
    bool catchup_failed = false;
    bool retry_pending = false;

    State(Executor executor, Scheduler scheduler, std::chrono::milliseconds interval,
          std::size_t retry_after_pulses, DiscoveryCycleCallbacks handlers)
        : timer(std::move(executor), std::move(scheduler)), period(interval),
          retry_delay(retry_after_pulses), callbacks(std::move(handlers)) {}
    void close();
    void fail(DiscoveryCycleError error);
    [[nodiscard]] bool begin_catchup();
    [[nodiscard]] bool watch_step();
    [[nodiscard]] bool lease_step();
    [[nodiscard]] bool pulse();
    [[nodiscard]] DiscoveryWatchCallbacks watch_handlers();
    [[nodiscard]] AdvertisementLeaseCallbacks lease_handlers();
};

void NativeDiscoveryCycle::State::close() {
    if (!active)
        return;
    active = false;
    timer.close();
    if (watch)
        watch->close();
    if (lease)
        lease->close();
}

void NativeDiscoveryCycle::State::fail(DiscoveryCycleError error) {
    if (!active)
        return;
    close();
    callbacks.on_failure(error);
}

bool NativeDiscoveryCycle::State::begin_catchup() {
    if (catchup_started)
        return true;
    if (watch->automatically_catches_up()) {
        catchup_started = true;
        return true;
    }
    auto registration = watch->registration();
    if (registration.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return true;
    try {
        if (registration.get() == 0) {
            fail(DiscoveryCycleError::Registration);
            return false;
        }
    } catch (const std::exception &) {
        fail(DiscoveryCycleError::Registration);
        return false;
    }
    try {
        if (!std::holds_alternative<std::monostate>(watch->catch_up())) {
            fail(DiscoveryCycleError::Catchup);
            return false;
        }
    } catch (const std::exception &) {
        fail(DiscoveryCycleError::Catchup);
        return false;
    }
    catchup_started = true;
    return true;
}

bool NativeDiscoveryCycle::State::watch_step() {
    if (!watch)
        return true;
    if (!watch->is_open()) {
        fail(DiscoveryCycleError::Watch);
        return false;
    }
    if (catchup_failed) {
        fail(DiscoveryCycleError::Catchup);
        return false;
    }
    if (!begin_catchup())
        return false;
    if (!watch->tick()) {
        fail(DiscoveryCycleError::Watch);
        return false;
    }
    return true;
}

bool NativeDiscoveryCycle::State::lease_step() {
    if (!lease)
        return true;
    if (!lease->is_open()) {
        fail(DiscoveryCycleError::Lease);
        return false;
    }
    const auto result = lease->tick();
    if (result == AdvertisementLeaseTick::Failed || result == AdvertisementLeaseTick::Closed) {
        fail(DiscoveryCycleError::Lease);
        return false;
    }
    if (result == AdvertisementLeaseTick::Pending && retry_pending) {
        if (retry_remaining > 0)
            --retry_remaining;
        if (retry_remaining == 0) {
            retry_pending = false;
            (void)lease->retry();
        }
    }
    if (!lease->is_open()) {
        fail(DiscoveryCycleError::Lease);
        return false;
    }
    return true;
}

bool NativeDiscoveryCycle::State::pulse() {
    if (!active)
        return false;
    if (!watch_step() || !lease_step())
        return false;
    try {
        timer.arm(period, [weak = weak_from_this()] {
            if (auto self = weak.lock())
                (void)self->pulse();
        });
        return true;
    } catch (const std::exception &) {
        fail(DiscoveryCycleError::Scheduler);
        return false;
    }
}

DiscoveryWatchCallbacks NativeDiscoveryCycle::State::watch_handlers() {
    auto result = callbacks.watch;
    result.on_catchup = [weak = weak_from_this()](bool ok) {
        if (auto self = weak.lock(); self && self->active) {
            self->catchup_failed = !ok;
            self->callbacks.watch.on_catchup(ok);
        }
    };
    return result;
}

AdvertisementLeaseCallbacks NativeDiscoveryCycle::State::lease_handlers() {
    auto result = callbacks.lease;
    result.on_error = [weak = weak_from_this()](AdvertisementLeaseError error) {
        if (auto self = weak.lock(); self && self->active) {
            if (std::holds_alternative<OutboxError>(error)) {
                self->retry_pending = true;
                self->retry_remaining = self->retry_delay;
            }
            self->callbacks.lease.on_error(std::move(error));
        }
    };
    return result;
}

NativeDiscoveryCycle::NativeDiscoveryCycle(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeDiscoveryCycle::~NativeDiscoveryCycle() { close(); }

std::variant<std::unique_ptr<NativeDiscoveryCycle>, StoreError, CryptoError>
NativeDiscoveryCycle::create(DiscoveryCycleConfig config, Executor executor, Scheduler scheduler,
                             DiscoveryCycleCallbacks callbacks) {
    if (!executor || !scheduler || !config.now || config.period.count() <= 0 ||
        !config.retry_after_pulses || !callbacks.on_failure ||
        (!config.watch_limits && !config.lease_config) ||
        (config.watch_limits && !callbacks.watch.on_catchup) ||
        (config.lease_config && !callbacks.lease.on_error)) {
        throw std::invalid_argument("invalid discovery cycle runtime");
    }
    auto state = std::make_shared<State>(executor, std::move(scheduler), config.period,
                                         config.retry_after_pulses, std::move(callbacks));
    if (config.watch_limits) {
        auto watch = NativeDiscoveryWatch::create(config.watch_store, config.capability,
                                                  *config.watch_limits, config.budget, executor,
                                                  config.now, state->watch_handlers());
        if (auto *error = std::get_if<StoreError>(&watch))
            return *error;
        if (auto *error = std::get_if<CryptoError>(&watch))
            return *error;
        state->watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(watch));
    }
    if (config.lease_config) {
        auto lease = NativeAdvertisementLease::create(
            config.lease_store, config.identity, config.capability, *config.lease_config,
            config.budget, std::move(executor), std::move(config.now), state->lease_handlers());
        if (auto *error = std::get_if<CryptoError>(&lease))
            return *error;
        state->lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(lease));
    }
    return std::unique_ptr<NativeDiscoveryCycle>(new NativeDiscoveryCycle(std::move(state)));
}

bool NativeDiscoveryCycle::start() {
    if (!state_->active || state_->started)
        return false;
    state_->started = true;
    return state_->pulse();
}
bool NativeDiscoveryCycle::is_open() const {
    return state_->active && state_->timer.healthy() && !state_->catchup_failed &&
           (!state_->watch || state_->watch->is_open()) &&
           (!state_->lease || state_->lease->is_open());
}
bool NativeDiscoveryCycle::watch_available() const {
    return is_open() && state_->watch && state_->watch->available();
}
bool NativeDiscoveryCycle::watch_recovering() const {
    return is_open() && state_->watch && state_->watch->recovering();
}
bool NativeDiscoveryCycle::is_current(const Advertisement &value) {
    return is_open() && state_->watch && state_->watch->is_current(value);
}
std::optional<std::vector<Advertisement>> NativeDiscoveryCycle::current_peers() {
    if (!is_open() || !state_->watch) {
        return std::nullopt;
    }
    return state_->watch->current_peers();
}
std::optional<AdvertisementFields> NativeDiscoveryCycle::published_fields() {
    if (!is_open() || !state_->lease) {
        return std::nullopt;
    }
    return state_->lease->published_fields();
}
void NativeDiscoveryCycle::stop_watching() {
    if (state_->watch) {
        state_->watch->close();
        state_->watch.reset();
        state_->catchup_failed = false;
    }
    if (!state_->lease)
        close();
}
void NativeDiscoveryCycle::stop_announcing() {
    if (state_->lease) {
        state_->lease->close();
        state_->lease.reset();
    }
    if (!state_->watch)
        close();
}
void NativeDiscoveryCycle::close() { state_->close(); }
} // namespace p2pdc::detail
