#include "native_signal_pipe.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace p2pdc::detail {

struct NativeSignalPipe::State {
    explicit State(MemoryBudget shared_budget) : budget(std::move(shared_budget)) {}
    SignalPipeCallbacks callbacks;
    std::unique_ptr<NativeMailbox> mailbox;
    std::vector<std::unique_ptr<NativeOutbox>> outboxes;
    OutboxLimits limits{};
    MemoryBudget budget;
    bool active = true;
    static OutboxCallbacks outgoing(std::weak_ptr<State> weak);
    static MailboxCallbacks incoming(std::weak_ptr<State> weak);
};

OutboxCallbacks NativeSignalPipe::State::outgoing(std::weak_ptr<State> weak) {
    return {[weak] {
                if (auto self = weak.lock(); self && self->active)
                    self->callbacks.on_published();
            },
            [weak](OutboxError error) {
                if (auto self = weak.lock(); self && self->active)
                    self->callbacks.on_outgoing_error(error);
            }};
}

MailboxCallbacks NativeSignalPipe::State::incoming(std::weak_ptr<State> weak) {
    return {[weak](Bytes record) {
                if (auto self = weak.lock(); self && self->active)
                    self->callbacks.on_record(std::move(record));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock(); self && self->active)
                    self->callbacks.on_incoming_error(error, detail);
            },
            [weak](bool success) {
                if (auto self = weak.lock(); self && self->active)
                    self->callbacks.on_catchup(success);
            }};
}

NativeSignalPipe::NativeSignalPipe(std::shared_ptr<State> state) : state_(std::move(state)) {}
NativeSignalPipe::~NativeSignalPipe() { close(); }

std::variant<std::unique_ptr<NativeSignalPipe>, StoreError>
NativeSignalPipe::create(RecordStore store, std::array<std::byte, 20> local_key,
                         std::array<std::byte, 20> remote_key, MemoryBudget budget,
                         SignalPipeLimits limits, Executor executor,
                         SignalPipeCallbacks callbacks) {
    if (!executor || !callbacks.on_record || !callbacks.on_incoming_error ||
        !callbacks.on_outgoing_error || !callbacks.on_published || !callbacks.on_catchup) {
        throw std::invalid_argument("native signal pipe requires executor and callbacks");
    }
    if (std::all_of(remote_key.begin(), remote_key.end(),
                    [](auto value) { return value == std::byte{}; })) {
        return StoreError::InvalidKey;
    }
    auto state = std::make_shared<State>(budget);
    state->callbacks = std::move(callbacks);
    const std::weak_ptr<State> weak = state;
    state->limits = limits.outgoing;
    const auto count = std::min<std::size_t>(8, limits.outgoing.queued_records);
    if (!count)
        throw std::invalid_argument("native signal pipe requires outgoing capacity");
    for (std::size_t index = 0; index < count; ++index)
        state->outboxes.push_back(std::make_unique<NativeOutbox>(
            store, remote_key, budget, limits.outgoing, executor, State::outgoing(weak), true));
    auto mailbox = NativeMailbox::create(store, local_key, limits.incoming, std::move(budget),
                                         std::move(executor), State::incoming(weak));
    if (auto *error = std::get_if<StoreError>(&mailbox)) {
        return *error;
    }
    state->mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(mailbox));
    return std::unique_ptr<NativeSignalPipe>(new NativeSignalPipe(std::move(state)));
}

std::shared_future<std::size_t> NativeSignalPipe::registration() const {
    return state_->mailbox->registration();
}
CatchupResult NativeSignalPipe::catch_up() {
    if (!is_open())
        return CatchupError::Closed;
    return state_->mailbox->catch_up();
}
SignalingQueueResult NativeSignalPipe::send(Bytes record) {
    if (!is_open())
        return SignalingQueueResult::Closed;
    if (record.size() > state_->limits.record_bytes)
        return SignalingQueueResult::TooLarge;
    std::size_t count = 0, bytes = 0;
    for (const auto &outbox : state_->outboxes) {
        count += outbox->size();
        bytes += outbox->buffered_bytes();
    }
    if (count >= state_->limits.queued_records ||
        record.capacity() > state_->limits.queued_bytes - bytes)
        return SignalingQueueResult::Full;
    const auto available = state_->budget.limit() - state_->budget.used();
    if (record.capacity() > available)
        return SignalingQueueResult::Full;
    const bool can_publish = record.size() <= available - record.capacity();
    NativeOutbox *selected = nullptr;
    for (const auto &outbox : state_->outboxes) {
        if ((can_publish || outbox->size() != 0) &&
            (!selected || outbox->size() < selected->size()))
            selected = outbox.get();
    }
    return selected ? selected->enqueue(std::move(record)) : SignalingQueueResult::Full;
}
bool NativeSignalPipe::retry() {
    if (!is_open())
        return false;
    bool retried = false;
    for (const auto &outbox : state_->outboxes)
        retried = outbox->retry() || retried;
    return retried;
}
void NativeSignalPipe::retry_budget() {
    if (!is_open())
        return;
    for (const auto &outbox : state_->outboxes)
        (void)outbox->retry_budget();
}
bool NativeSignalPipe::is_open() const {
    return state_->active && state_->mailbox->is_open() &&
           std::all_of(state_->outboxes.begin(), state_->outboxes.end(),
                       [](const auto &outbox) { return outbox->is_open(); });
}
void NativeSignalPipe::close() {
    state_->active = false;
    state_->mailbox->close();
    for (const auto &outbox : state_->outboxes)
        outbox->close();
}

} // namespace p2pdc::detail
