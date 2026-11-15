#include "recovering_mailbox.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
struct RecoveryFailure {
    RecoveryError error;
    bool retryable;
};
struct RecoveringMailbox::State : std::enable_shared_from_this<State> {
    State(RecordStore source, std::array<std::byte, 20> address, MailboxLimits bounds,
          MemoryBudget memory, Executor dispatch, RecoveryOptions settings,
          RecoveryCallbacks handlers, LookupDeduplication dedup)
        : store(std::move(source)), key(address), limits(bounds), budget(std::move(memory)),
          executor(std::move(dispatch)), options(settings), callbacks(std::move(handlers)),
          deduplication(dedup) {}
    [[nodiscard]] MailboxCallbacks handlers(std::uint64_t generation);
    void open();
    void catchup();
    void failed(RecoveryFailure failure);
    void handle_failure(Clock::time_point now);
    void finish(RecoveryError error);
    void close();
    [[nodiscard]] bool step(Clock::time_point now);
    [[nodiscard]] std::chrono::milliseconds retry_delay() const;
    RecordStore store;
    std::array<std::byte, 20> key;
    MailboxLimits limits;
    MemoryBudget budget;
    Executor executor;
    RecoveryOptions options;
    RecoveryCallbacks callbacks;
    LookupDeduplication deduplication;
    std::unique_ptr<NativeMailbox> mailbox;
    std::optional<RecoveryFailure> pending;
    std::optional<Clock::time_point> last_poll;
    Clock::time_point retry_at{};
    RecoveryState phase = RecoveryState::Opening;
    std::size_t retries = 0;
    std::uint64_t generation = 0;
    bool reported_reconnecting = false;
};

namespace {
void validate(ByteView key, MailboxLimits limits, const Executor &executor, RecoveryOptions options,
              const RecoveryCallbacks &callbacks, LookupDeduplication dedup) {
    if (key.size() != 20 ||
        std::all_of(key.begin(), key.end(), [](std::byte b) { return b == std::byte{}; }) ||
        !limits.queued_records || limits.record_bytes > limits.queued_bytes || !executor ||
        !callbacks.on_record || !callbacks.on_catchup || !callbacks.on_status ||
        !callbacks.on_error || options.max_retries > 10 || options.initial_delay.count() < 10 ||
        options.max_delay < options.initial_delay || options.max_delay > std::chrono::minutes(1) ||
        (dedup != LookupDeduplication::Content && dedup != LookupDeduplication::IdAndContent))
        throw std::invalid_argument("invalid recovering mailbox settings");
}
RecoveryFailure classify(MailboxError error, std::optional<StoreError> detail) {
    if (error == MailboxError::StoreFailure && detail == StoreError::NotRunning)
        return {RecoveryError::Backend, true};
    if (error != MailboxError::StoreFailure || detail == StoreError::ResourceLimit ||
        detail == StoreError::AllocationFailed)
        return {RecoveryError::ResourceLimit, false};
    return {RecoveryError::Backend, false};
}
} // namespace

MailboxCallbacks RecoveringMailbox::State::handlers(std::uint64_t current) {
    auto weak = weak_from_this();
    return {[weak, current](Bytes value) {
                if (auto self = weak.lock();
                    self && self->generation == current && !self->pending &&
                    self->phase != RecoveryState::Closed && self->phase != RecoveryState::Failed)
                    self->callbacks.on_record(std::move(value));
            },
            [weak, current](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock(); self && self->generation == current)
                    self->failed(classify(error, detail));
            },
            [weak, current](bool success) {
                if (auto self = weak.lock();
                    self && self->generation == current && !self->pending) {
                    if (!success)
                        self->failed({RecoveryError::Backend, true});
                    else {
                        self->phase = RecoveryState::Ready;
                        self->retries = 0;
                        self->reported_reconnecting = false;
                        self->callbacks.on_status(RecoveryState::Ready);
                        self->callbacks.on_catchup();
                    }
                }
            }};
}
void RecoveringMailbox::State::failed(RecoveryFailure failure) {
    if (phase != RecoveryState::Closed && phase != RecoveryState::Failed && !pending)
        pending = failure;
}
void RecoveringMailbox::State::finish(RecoveryError error) {
    if (phase == RecoveryState::Closed || phase == RecoveryState::Failed)
        return;
    phase = RecoveryState::Failed;
    if (mailbox) {
        mailbox->close();
        mailbox.reset();
    }
    ++generation;
    callbacks.on_error(error);
}
void RecoveringMailbox::State::close() {
    if (phase == RecoveryState::Closed)
        return;
    phase = RecoveryState::Closed;
    pending.reset();
    ++generation;
    if (mailbox) {
        mailbox->close();
        mailbox.reset();
    }
}
std::chrono::milliseconds RecoveringMailbox::State::retry_delay() const {
    auto delay = options.initial_delay;
    for (std::size_t index = 1; index < retries; ++index)
        delay = std::min(options.max_delay, delay * 2);
    return delay;
}
void RecoveringMailbox::State::handle_failure(Clock::time_point now) {
    const auto failure = *pending;
    pending.reset();
    if (mailbox) {
        mailbox->close();
        mailbox.reset();
    }
    ++generation;
    if (!failure.retryable || retries >= options.max_retries) {
        finish(failure.error);
        return;
    }
    ++retries;
    phase = RecoveryState::Reconnecting;
    const auto delay = retry_delay();
    retry_at = now > Clock::time_point::max() - delay ? Clock::time_point::max() : now + delay;
    if (!reported_reconnecting) {
        reported_reconnecting = true;
        callbacks.on_status(RecoveryState::Reconnecting);
    }
}
void RecoveringMailbox::State::open() {
    phase = RecoveryState::Opening;
    auto result = NativeMailbox::create(store, key, limits, budget, executor,
                                        handlers(++generation), deduplication);
    if (auto *error = std::get_if<StoreError>(&result)) {
        failed({*error == StoreError::ResourceLimit || *error == StoreError::AllocationFailed
                    ? RecoveryError::ResourceLimit
                    : RecoveryError::Backend,
                *error == StoreError::NotRunning});
        return;
    }
    mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(result));
}
void RecoveringMailbox::State::catchup() {
    auto registration = mailbox->registration();
    if (registration.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try {
        if (!registration.get()) {
            const auto error = mailbox->pending_store_error();
            failed(error ? classify(MailboxError::StoreFailure, error)
                         : RecoveryFailure{RecoveryError::Backend, true});
            return;
        }
        auto result = mailbox->catch_up();
        if (auto *error = std::get_if<StoreError>(&result)) {
            failed({*error == StoreError::ResourceLimit || *error == StoreError::AllocationFailed
                        ? RecoveryError::ResourceLimit
                        : RecoveryError::Backend,
                    *error == StoreError::NotRunning});
            return;
        }
        if (!std::holds_alternative<std::monostate>(result)) {
            failed({RecoveryError::Backend, false});
            return;
        }
        phase = RecoveryState::CatchingUp;
    } catch (const std::bad_alloc &) {
        failed({RecoveryError::ResourceLimit, false});
    } catch (const std::exception &) {
        failed({RecoveryError::Backend, false});
    }
}
bool RecoveringMailbox::State::step(Clock::time_point now) {
    if (phase == RecoveryState::Closed || phase == RecoveryState::Failed)
        return false;
    if (last_poll && now < *last_poll) {
        finish(RecoveryError::Clock);
        return false;
    }
    last_poll = now;
    if (pending)
        handle_failure(now);
    if (phase == RecoveryState::Failed || phase == RecoveryState::Closed)
        return false;
    if (phase == RecoveryState::Reconnecting && now < retry_at)
        return true;
    if (!mailbox)
        open();
    if (pending) {
        handle_failure(now);
        return phase != RecoveryState::Failed;
    }
    if (phase == RecoveryState::Opening)
        catchup();
    if (pending) {
        handle_failure(now);
        return phase != RecoveryState::Failed;
    }
    if (!mailbox->is_open()) {
        finish(RecoveryError::Backend);
        return false;
    }
    return true;
}
RecoveringMailbox::RecoveringMailbox(RecordStore store, std::array<std::byte, 20> key,
                                     MailboxLimits limits, MemoryBudget budget, Executor executor,
                                     RecoveryOptions options, RecoveryCallbacks callbacks,
                                     LookupDeduplication deduplication) {
    validate(key, limits, executor, options, callbacks, deduplication);
    state_ =
        std::make_shared<State>(std::move(store), key, limits, std::move(budget),
                                std::move(executor), options, std::move(callbacks), deduplication);
}
RecoveringMailbox::~RecoveringMailbox() { close(); }
bool RecoveringMailbox::poll(Clock::time_point now) {
    try {
        return state_->step(now);
    } catch (const std::bad_alloc &) {
        state_->finish(RecoveryError::ResourceLimit);
    } catch (const std::exception &) {
        state_->finish(RecoveryError::Backend);
    }
    return false;
}
bool RecoveringMailbox::is_open() const {
    return state_->phase != RecoveryState::Failed && state_->phase != RecoveryState::Closed;
}
RecoveryState RecoveringMailbox::state() const {
    if (state_->pending)
        return state_->pending->retryable && state_->retries < state_->options.max_retries
                   ? RecoveryState::Reconnecting
                   : RecoveryState::Failed;
    return state_->phase;
}
void RecoveringMailbox::close() {
    if (state_)
        state_->close();
}
} // namespace p2pdc::detail
