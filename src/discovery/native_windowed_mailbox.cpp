#include "native_windowed_mailbox.hpp"

#include <chrono>
#include <map>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
struct NativeWindowedMailbox::State : std::enable_shared_from_this<State> {
    struct Window {
        std::unique_ptr<NativeMailbox> mailbox;
        std::unique_ptr<RecoveringMailbox> recovery;
        bool started = false;
        std::optional<bool> result{};
        bool unavailable = false;
    };
    RecordStore store;
    std::shared_ptr<const Bytes> capability;
    MailboxLimits limits;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
    MemoryBudget budget;
    Executor executor;
    WindowedMailboxCallbacks callbacks;
    std::optional<RecoveryOptions> recovery_options;
    std::map<std::uint64_t, Window> windows;
    std::optional<DiscoveryWindowRange> range;
    std::optional<std::uint64_t> last_now;
    bool active = true;
    bool healthy = true;
    bool reported = false;
    bool reported_reconnecting = false;

    State(RecordStore source, std::shared_ptr<const Bytes> secret, MailboxLimits bounds,
          std::uint64_t lifetime, std::uint64_t skew, MemoryBudget memory, Executor dispatch,
          WindowedMailboxCallbacks handlers, std::optional<RecoveryOptions> recovery)
        : store(std::move(source)), capability(std::move(secret)), limits(bounds),
          max_lifetime(lifetime), clock_skew(skew), budget(std::move(memory)),
          executor(std::move(dispatch)), callbacks(std::move(handlers)),
          recovery_options(recovery) {}
    void fail(WindowedMailboxError error, std::optional<StoreError> detail = std::nullopt);
    void report();
    [[nodiscard]] MailboxCallbacks handlers(std::uint64_t window);
    [[nodiscard]] RecoveryCallbacks recovery_handlers(std::uint64_t window);
    void status(std::uint64_t window, RecoveryState value);
    void update_status();
    [[nodiscard]] bool add(std::uint64_t window);
    [[nodiscard]] bool reconcile(DiscoveryWindowRange next);
    [[nodiscard]] bool start_lookups();
    [[nodiscard]] bool tick(std::uint64_t now);
};

void NativeWindowedMailbox::State::fail(WindowedMailboxError error,
                                        std::optional<StoreError> detail) {
    if (active && healthy) {
        healthy = false;
        callbacks.on_error(error, detail);
    }
}

void NativeWindowedMailbox::State::report() {
    if (!active || !healthy || reported || windows.empty())
        return;
    bool success = true;
    for (const auto &[window, entry] : windows) {
        (void)window;
        if (entry.recovery && entry.recovery->state() != RecoveryState::Ready)
            return;
        if (!entry.result)
            return;
        success = success && *entry.result;
    }
    reported = true;
    callbacks.on_catchup(success);
}

MailboxCallbacks NativeWindowedMailbox::State::handlers(std::uint64_t window) {
    auto weak = weak_from_this();
    return {[weak](Bytes wire) {
                if (auto self = weak.lock(); self && self->active && self->healthy)
                    self->callbacks.on_record(std::move(wire));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock())
                    self->fail(error == MailboxError::StoreFailure
                                   ? WindowedMailboxError::Store
                                   : WindowedMailboxError::ResourceLimit,
                               detail);
            },
            [weak, window](bool success) {
                if (auto self = weak.lock(); self && self->active && self->healthy) {
                    auto found = self->windows.find(window);
                    if (found != self->windows.end()) {
                        found->second.result = success;
                        self->report();
                    }
                }
            }};
}

RecoveryCallbacks NativeWindowedMailbox::State::recovery_handlers(std::uint64_t window) {
    auto weak = weak_from_this();
    return {[weak](Bytes wire) {
                if (auto self = weak.lock(); self && self->active && self->healthy)
                    self->callbacks.on_record(std::move(wire));
            },
            [weak, window] {
                if (auto self = weak.lock(); self && self->active && self->healthy) {
                    auto found = self->windows.find(window);
                    if (found != self->windows.end()) {
                        found->second.result = true;
                        self->report();
                    }
                }
            },
            [weak, window](RecoveryState value) {
                if (auto self = weak.lock())
                    self->status(window, value);
            },
            [weak](RecoveryError error) {
                if (auto self = weak.lock())
                    self->fail(
                        error == RecoveryError::ResourceLimit ? WindowedMailboxError::ResourceLimit
                        : error == RecoveryError::Clock       ? WindowedMailboxError::ClockRegressed
                                                              : WindowedMailboxError::Store);
            }};
}
void NativeWindowedMailbox::State::status(std::uint64_t window, RecoveryState value) {
    auto found = windows.find(window);
    if (!active || !healthy || found == windows.end())
        return;
    if (value == RecoveryState::Reconnecting) {
        found->second.unavailable = true;
        found->second.result.reset();
        reported = false;
    } else if (value == RecoveryState::Ready) {
        found->second.unavailable = false;
    }
    update_status();
}
void NativeWindowedMailbox::State::update_status() {
    if (!active || !healthy || windows.empty())
        return;
    bool unavailable = false, all_ready = true;
    for (const auto &[unused, entry] : windows) {
        (void)unused;
        if (!entry.recovery)
            continue;
        const auto value = entry.recovery->state();
        unavailable = unavailable || entry.unavailable || value == RecoveryState::Reconnecting;
        all_ready = all_ready && value == RecoveryState::Ready;
    }
    if (unavailable && !reported_reconnecting) {
        reported_reconnecting = true;
        if (callbacks.on_status)
            callbacks.on_status(RecoveryState::Reconnecting);
    } else if (reported_reconnecting && !unavailable && all_ready) {
        reported_reconnecting = false;
        if (callbacks.on_status)
            callbacks.on_status(RecoveryState::Ready);
    }
}

bool NativeWindowedMailbox::State::add(std::uint64_t window) {
    auto key = derive_discovery_window_key(*capability, window);
    if (std::holds_alternative<CryptoError>(key)) {
        fail(WindowedMailboxError::Crypto);
        return false;
    }
    if (recovery_options) {
        auto mailbox = std::make_unique<RecoveringMailbox>(
            store, std::get<DiscoveryKey>(key), limits, budget, executor, *recovery_options,
            recovery_handlers(window));
        windows.emplace(window, Window{{}, std::move(mailbox)});
        return true;
    }
    auto mailbox = NativeMailbox::create(store, std::get<DiscoveryKey>(key), limits, budget,
                                         executor, handlers(window), LookupDeduplication::Content);
    if (auto *error = std::get_if<StoreError>(&mailbox)) {
        fail(WindowedMailboxError::Store, *error);
        return false;
    }
    windows.emplace(window,
                    Window{std::move(std::get<std::unique_ptr<NativeMailbox>>(mailbox)), {}});
    return true;
}

bool NativeWindowedMailbox::State::reconcile(DiscoveryWindowRange next) {
    if (range && *range == next)
        return true;
    for (auto window = next.first;; ++window) {
        if (!windows.contains(window) && !add(window))
            return false;
        if (window == next.last)
            break;
    }
    for (auto it = windows.begin(); it != windows.end();) {
        if (it->first < next.first || it->first > next.last) {
            if (it->second.mailbox)
                it->second.mailbox->close();
            if (it->second.recovery)
                it->second.recovery->close();
            it = windows.erase(it);
        } else {
            ++it;
        }
    }
    range = next;
    reported = false;
    return true;
}

bool NativeWindowedMailbox::State::start_lookups() {
    for (auto &[window, entry] : windows) {
        (void)window;
        if (entry.recovery) {
            if (!entry.recovery->poll(RecoveringMailbox::Clock::now()))
                return false;
            continue;
        }
        if (!entry.mailbox->is_open()) {
            fail(WindowedMailboxError::Store);
            return false;
        }
        if (entry.started)
            continue;
        auto registration = entry.mailbox->registration();
        if (registration.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;
        if (registration.get() == 0) {
            fail(WindowedMailboxError::Registration);
            return false;
        }
        if (!std::holds_alternative<std::monostate>(entry.mailbox->catch_up())) {
            fail(WindowedMailboxError::Catchup);
            return false;
        }
        entry.started = true;
    }
    report();
    update_status();
    return true;
}

bool NativeWindowedMailbox::State::tick(std::uint64_t now) {
    if (!active || !healthy)
        return false;
    if (last_now && now < *last_now) {
        fail(WindowedMailboxError::ClockRegressed);
        return false;
    }
    last_now = now;
    auto next = discovery_window_range(now, max_lifetime, clock_skew);
    if (!std::holds_alternative<DiscoveryWindowRange>(next)) {
        fail(WindowedMailboxError::InvalidRange);
        return false;
    }
    try {
        return reconcile(std::get<DiscoveryWindowRange>(next)) && start_lookups();
    } catch (const std::bad_alloc &) {
        fail(WindowedMailboxError::ResourceLimit);
        return false;
    } catch (const std::exception &) {
        fail(WindowedMailboxError::Store);
        return false;
    }
}

NativeWindowedMailbox::NativeWindowedMailbox(RecordStore store,
                                             std::shared_ptr<const Bytes> capability,
                                             MailboxLimits limits, std::uint64_t max_lifetime,
                                             std::uint64_t clock_skew, MemoryBudget budget,
                                             Executor executor, WindowedMailboxCallbacks callbacks,
                                             std::optional<RecoveryOptions> recovery) {
    if (!capability || capability->size() != capability_size || !max_lifetime || !executor ||
        !callbacks.on_record || !callbacks.on_error || !callbacks.on_catchup)
        throw std::invalid_argument("invalid windowed mailbox configuration");
    state_ = std::make_shared<State>(std::move(store), std::move(capability), limits, max_lifetime,
                                     clock_skew, std::move(budget), std::move(executor),
                                     std::move(callbacks), recovery);
}
NativeWindowedMailbox::~NativeWindowedMailbox() { close(); }
bool NativeWindowedMailbox::tick(std::uint64_t now) { return state_->tick(now); }
bool NativeWindowedMailbox::is_open() const {
    if (!state_->active || !state_->healthy)
        return false;
    for (const auto &[window, entry] : state_->windows) {
        (void)window;
        if (entry.mailbox && !entry.mailbox->is_open())
            return false;
        if (entry.recovery && !entry.recovery->is_open())
            return false;
    }
    return true;
}
bool NativeWindowedMailbox::available() const {
    if (!is_open() || state_->reported_reconnecting)
        return false;
    for (const auto &[unused, entry] : state_->windows) {
        (void)unused;
        if (entry.unavailable)
            return false;
        if (entry.recovery && (entry.recovery->state() == RecoveryState::Reconnecting ||
                               entry.recovery->state() == RecoveryState::Failed))
            return false;
    }
    return true;
}
bool NativeWindowedMailbox::recovering() const {
    if (!is_open())
        return false;
    if (state_->reported_reconnecting)
        return true;
    for (const auto &[unused, entry] : state_->windows) {
        (void)unused;
        if (entry.unavailable ||
            (entry.recovery && entry.recovery->state() == RecoveryState::Reconnecting))
            return true;
    }
    return false;
}
void NativeWindowedMailbox::close() {
    state_->active = false;
    for (auto &[window, entry] : state_->windows) {
        (void)window;
        if (entry.mailbox)
            entry.mailbox->close();
        if (entry.recovery)
            entry.recovery->close();
    }
    state_->windows.clear();
}
} // namespace p2pdc::detail
