#include "native_discovery_watch.hpp"
#include "native_windowed_mailbox.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

struct NativeDiscoveryWatch::State : std::enable_shared_from_this<State> {
    std::shared_ptr<const Bytes> capability;
    DiscoveryWatchLimits limits;
    AdvertisementTracker tracker;
    std::function<std::uint64_t()> now;
    DiscoveryWatchCallbacks callbacks;
    std::unique_ptr<NativeMailbox> mailbox;
    std::unique_ptr<RecoveringMailbox> recovery_mailbox;
    std::unique_ptr<NativeWindowedMailbox> windowed_mailbox;
    bool active = true;
    bool healthy = true;
    bool unavailable = false;
    State(std::shared_ptr<const Bytes> secret, DiscoveryWatchLimits bounds,
          std::function<std::uint64_t()> clock, DiscoveryWatchCallbacks handlers)
        : capability(std::move(secret)), limits(bounds),
          tracker(bounds.max_entries, bounds.max_lifetime, bounds.clock_skew),
          now(std::move(clock)), callbacks(std::move(handlers)) {}
    void fail(DiscoveryWatchError error, std::optional<StoreError> detail = std::nullopt);
    [[nodiscard]] bool sweep(std::uint64_t time);
    void receive(Bytes wire);
    [[nodiscard]] MailboxCallbacks handlers();
    [[nodiscard]] RecoveryCallbacks recovery_handlers();
    [[nodiscard]] WindowedMailboxCallbacks windowed_handlers();
};

void NativeDiscoveryWatch::State::fail(DiscoveryWatchError error,
                                       std::optional<StoreError> detail) {
    if (active && healthy) {
        healthy = false;
        callbacks.on_error(error, detail);
    }
}

RecoveryCallbacks NativeDiscoveryWatch::State::recovery_handlers() {
    auto weak = weak_from_this();
    return {[weak](Bytes wire) {
                if (auto self = weak.lock())
                    self->receive(std::move(wire));
            },
            [weak] {
                if (auto self = weak.lock(); self && self->active && self->healthy)
                    self->callbacks.on_catchup(true);
            },
            [weak](RecoveryState value) {
                if (auto self = weak.lock(); self && self->active && self->healthy) {
                    if (value == RecoveryState::Ready && !self->unavailable)
                        return;
                    self->unavailable = value == RecoveryState::Reconnecting;
                    if (self->callbacks.on_status)
                        self->callbacks.on_status(value);
                }
            },
            [weak](RecoveryError error) {
                if (auto self = weak.lock())
                    self->fail(error == RecoveryError::ResourceLimit
                                   ? DiscoveryWatchError::ResourceLimit
                               : error == RecoveryError::Clock ? DiscoveryWatchError::ClockRegressed
                                                               : DiscoveryWatchError::Store);
            }};
}
bool NativeDiscoveryWatch::State::sweep(std::uint64_t time) {
    auto result = tracker.sweep(time);
    if (std::holds_alternative<AdvertisementSweepError>(result)) {
        fail(DiscoveryWatchError::ClockRegressed);
        return false;
    }
    for (auto &value : std::get<std::vector<Advertisement>>(result)) {
        callbacks.on_expired(std::move(value));
    }
    return true;
}

void NativeDiscoveryWatch::State::receive(Bytes wire) {
    if (!active || !healthy)
        return;
    try {
        const auto time = now();
        if (!sweep(time))
            return;
        auto decoded =
            decode_advertisement(wire, *capability, {time, limits.max_lifetime, limits.clock_skew});
        if (auto *value = std::get_if<Advertisement>(&decoded)) {
            const auto event = tracker.observe(*value, time);
            if (event == AdvertisementEvent::Seen || event == AdvertisementEvent::Updated)
                callbacks.on_peer(std::move(*value), event);
            else if (event == AdvertisementEvent::Full)
                fail(DiscoveryWatchError::ResourceLimit);
            else if (event == AdvertisementEvent::ClockRegressed)
                fail(DiscoveryWatchError::ClockRegressed);
        } else if (auto *error = std::get_if<CryptoError>(&decoded);
                   error && (*error == CryptoError::InitializationFailed ||
                             *error == CryptoError::AllocationFailed ||
                             *error == CryptoError::InvalidIdentity ||
                             *error == CryptoError::OperationFailed)) {
            fail(DiscoveryWatchError::CryptoFailure);
        }
    } catch (const std::exception &) {
        fail(DiscoveryWatchError::CryptoFailure);
    }
}

MailboxCallbacks NativeDiscoveryWatch::State::handlers() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                if (auto self = weak.lock())
                    self->receive(std::move(value));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock())
                    self->fail(error == MailboxError::StoreFailure
                                   ? DiscoveryWatchError::Store
                                   : DiscoveryWatchError::ResourceLimit,
                               detail);
            },
            [weak](bool success) {
                if (auto self = weak.lock(); self && self->active && self->healthy)
                    self->callbacks.on_catchup(success);
            }};
}

WindowedMailboxCallbacks NativeDiscoveryWatch::State::windowed_handlers() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                if (auto self = weak.lock())
                    self->receive(std::move(value));
            },
            [weak](WindowedMailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock()) {
                    const auto mapped = error == WindowedMailboxError::ClockRegressed
                                            ? DiscoveryWatchError::ClockRegressed
                                        : error == WindowedMailboxError::InvalidRange ||
                                                error == WindowedMailboxError::ResourceLimit
                                            ? DiscoveryWatchError::ResourceLimit
                                        : error == WindowedMailboxError::Crypto
                                            ? DiscoveryWatchError::CryptoFailure
                                            : DiscoveryWatchError::Store;
                    self->fail(mapped, detail);
                }
            },
            [weak](bool success) {
                if (auto self = weak.lock(); self && self->active && self->healthy)
                    self->callbacks.on_catchup(success);
            },
            [weak](RecoveryState value) {
                if (auto self = weak.lock(); self && self->active && self->healthy) {
                    self->unavailable = value == RecoveryState::Reconnecting;
                    if (self->callbacks.on_status)
                        self->callbacks.on_status(value);
                }
            }};
}

NativeDiscoveryWatch::NativeDiscoveryWatch(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeDiscoveryWatch::~NativeDiscoveryWatch() { close(); }
std::variant<std::unique_ptr<NativeDiscoveryWatch>, StoreError, CryptoError>
NativeDiscoveryWatch::create(RecordStore store, std::shared_ptr<const Bytes> capability,
                             DiscoveryWatchLimits limits, MemoryBudget budget, Executor executor,
                             std::function<std::uint64_t()> now,
                             DiscoveryWatchCallbacks callbacks) {
    if (!executor || !now || !capability || !callbacks.on_peer || !callbacks.on_expired ||
        !callbacks.on_error || !callbacks.on_catchup ||
        limits.mailbox.record_bytes < advertisement_wire_size) {
        throw std::invalid_argument(
            "native discovery watch requires executor, clock, callbacks and valid record limit");
    }
    auto state = std::make_shared<State>(std::move(capability), limits, std::move(now),
                                         std::move(callbacks));
    if (limits.windowed) {
        auto key = derive_discovery_window_key(*state->capability, 0);
        if (auto *error = std::get_if<CryptoError>(&key))
            return *error;
        state->windowed_mailbox = std::make_unique<NativeWindowedMailbox>(
            std::move(store), state->capability, limits.mailbox, limits.max_lifetime,
            limits.clock_skew, std::move(budget), std::move(executor), state->windowed_handlers(),
            limits.recovery);
        return std::unique_ptr<NativeDiscoveryWatch>(new NativeDiscoveryWatch(std::move(state)));
    }
    auto key = derive_discovery_key(*state->capability);
    if (auto *error = std::get_if<CryptoError>(&key))
        return *error;
    if (limits.recovery) {
        state->recovery_mailbox = std::make_unique<RecoveringMailbox>(
            std::move(store), std::get<DiscoveryKey>(key), limits.mailbox, std::move(budget),
            std::move(executor), *limits.recovery, state->recovery_handlers());
        return std::unique_ptr<NativeDiscoveryWatch>(new NativeDiscoveryWatch(std::move(state)));
    }
    auto mailbox = NativeMailbox::create(std::move(store), std::get<DiscoveryKey>(key),
                                         limits.mailbox, std::move(budget), std::move(executor),
                                         state->handlers(), LookupDeduplication::Content);
    if (auto *error = std::get_if<StoreError>(&mailbox))
        return *error;
    state->mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(mailbox));
    return std::unique_ptr<NativeDiscoveryWatch>(new NativeDiscoveryWatch(std::move(state)));
}
std::shared_future<std::size_t> NativeDiscoveryWatch::registration() const {
    if (state_->windowed_mailbox || state_->recovery_mailbox)
        return {};
    return state_->mailbox->registration();
}
bool NativeDiscoveryWatch::automatically_catches_up() const {
    return static_cast<bool>(state_->windowed_mailbox || state_->recovery_mailbox);
}
CatchupResult NativeDiscoveryWatch::catch_up() {
    if (!is_open())
        return CatchupError::Closed;
    if (state_->windowed_mailbox || state_->recovery_mailbox)
        return CatchupError::AlreadyStarted;
    return state_->mailbox->catch_up();
}
bool NativeDiscoveryWatch::tick() {
    if (!is_open())
        return false;
    try {
        const auto now = state_->now();
        return state_->sweep(now) &&
               (!state_->windowed_mailbox || state_->windowed_mailbox->tick(now)) &&
               (!state_->recovery_mailbox ||
                state_->recovery_mailbox->poll(RecoveringMailbox::Clock::now()));
    } catch (const std::exception &) {
        state_->fail(DiscoveryWatchError::CryptoFailure);
        return false;
    }
}
bool NativeDiscoveryWatch::is_open() const {
    if (!state_->active || !state_->healthy)
        return false;
    if (state_->windowed_mailbox)
        return state_->windowed_mailbox->is_open();
    if (state_->recovery_mailbox)
        return state_->recovery_mailbox->is_open();
    return state_->mailbox->is_open();
}
bool NativeDiscoveryWatch::available() const {
    if (!is_open() || state_->unavailable)
        return false;
    if (state_->windowed_mailbox)
        return state_->windowed_mailbox->available();
    if (!state_->recovery_mailbox)
        return true;
    const auto current = state_->recovery_mailbox->state();
    return current != RecoveryState::Reconnecting && current != RecoveryState::Failed;
}
bool NativeDiscoveryWatch::recovering() const {
    if (!is_open())
        return false;
    if (state_->unavailable)
        return true;
    if (state_->windowed_mailbox)
        return state_->windowed_mailbox->recovering();
    return state_->recovery_mailbox &&
           state_->recovery_mailbox->state() == RecoveryState::Reconnecting;
}
bool NativeDiscoveryWatch::is_current(const Advertisement &value) {
    if (!available()) {
        return false;
    }
    try {
        auto result = state_->tracker.is_current(value, state_->now());
        if (std::holds_alternative<AdvertisementSweepError>(result)) {
            state_->fail(DiscoveryWatchError::ClockRegressed);
            return false;
        }
        return std::get<bool>(result);
    } catch (const std::exception &) {
        state_->fail(DiscoveryWatchError::CryptoFailure);
        return false;
    }
}
std::optional<std::vector<Advertisement>> NativeDiscoveryWatch::current_peers() {
    if (!available()) {
        return std::nullopt;
    }
    try {
        auto result = state_->tracker.current_peers(state_->now());
        if (std::holds_alternative<AdvertisementSweepError>(result)) {
            state_->fail(DiscoveryWatchError::ClockRegressed);
            return std::nullopt;
        }
        return std::get<std::vector<Advertisement>>(std::move(result));
    } catch (const std::bad_alloc &) {
        state_->fail(DiscoveryWatchError::ResourceLimit, StoreError::AllocationFailed);
        return std::nullopt;
    } catch (const std::exception &) {
        state_->fail(DiscoveryWatchError::CryptoFailure);
        return std::nullopt;
    }
}
void NativeDiscoveryWatch::close() {
    state_->active = false;
    if (state_->windowed_mailbox)
        state_->windowed_mailbox->close();
    else if (state_->recovery_mailbox)
        state_->recovery_mailbox->close();
    else
        state_->mailbox->close();
}

} // namespace p2pdc::detail
