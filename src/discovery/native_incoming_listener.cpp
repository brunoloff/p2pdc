#include "native_incoming_listener.hpp"
#include "signal_errors.hpp"

#include <sodium.h>

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::size_t wire_limit = 32768;
constexpr std::size_t pending_limit = 16;
constexpr std::size_t pending_bytes_limit = 262144;
bool same_context(const IncomingContext &base, const IncomingContext &value, std::uint64_t now) {
    const auto &local = value.local;
    return local.endpoint.identity && local.endpoint.identity->signing &&
           local.endpoint.identity->recipient && local.capability && value.remote.capability &&
           local.capability->size() == capability_size &&
           value.remote.capability->size() == capability_size &&
           sodium_memcmp(local.capability->data(), value.remote.capability->data(),
                         capability_size) == 0 &&
           sodium_memcmp(local.capability->data(), base.local.capability->data(),
                         capability_size) == 0 &&
           local.endpoint.identity->signing->public_key() ==
               base.local.endpoint.identity->signing->public_key() &&
           local.endpoint.identity->recipient->public_key() ==
               base.local.endpoint.identity->recipient->public_key() &&
           local.endpoint.fields.instance_id == base.local.endpoint.fields.instance_id &&
           local.endpoint.fields.signaling_inbox == base.local.endpoint.fields.signaling_inbox &&
           local.endpoint.fields.recipient_public_key ==
               local.endpoint.identity->recipient->public_key() &&
           local.endpoint.fields.issued_at <= now && now < local.endpoint.fields.expires_at &&
           value.remote.max_lifetime == base.remote.max_lifetime &&
           value.remote.clock_skew == base.remote.clock_skew && value.remote.peers.size() <= 1024;
}
IncomingContext freeze_context(IncomingContext value) {
    auto identity = std::make_shared<const EndpointIdentity>(*value.local.endpoint.identity);
    auto capability = std::make_shared<const Bytes>(*value.local.capability);
    value.local.endpoint.identity = std::move(identity);
    value.local.capability = capability;
    value.remote.capability = std::move(capability);
    value.remote.peers.clear();
    return value;
}
bool fatal_crypto(CryptoError error) {
    return error == CryptoError::InitializationFailed || error == CryptoError::AllocationFailed ||
           error == CryptoError::InvalidIdentity || error == CryptoError::OperationFailed;
}
} // namespace

struct NativeIncomingListener::State : std::enable_shared_from_this<State> {
    struct PendingOffer {
        Bytes wire;
        MemoryReservation reservation;
        std::uint64_t received;
    };
    IncomingContext initial;
    SigningPublicKey signing_key;
    RecipientBoxPublicKey box_key;
    std::function<std::optional<IncomingContext>()> context;
    std::function<std::uint64_t()> now;
    IncomingListenerCallbacks callbacks;
    IncomingOfferResolver resolver;
    MemoryBudget budget;
    std::deque<PendingOffer> pending;
    std::size_t pending_bytes = 0;
    std::unique_ptr<NativeMailbox> mailbox;
    bool active = true;
    bool healthy = true;
    bool catchup_started = false;
    State(IncomingContext value, std::function<std::optional<IncomingContext>()> provider,
          std::function<std::uint64_t()> clock, MemoryBudget memory,
          IncomingListenerCallbacks handlers, std::shared_ptr<IncomingOfferReplay> replay)
        : initial(std::move(value)), context(std::move(provider)), now(std::move(clock)),
          callbacks(std::move(handlers)),
          resolver({32, 60, initial.remote.max_lifetime, initial.remote.clock_skew,
                    wire_limit - signaling_wire_overhead, 63},
                   std::move(replay)),
          budget(std::move(memory)) {
        signing_key = initial.local.endpoint.identity->signing->public_key();
        box_key = initial.local.endpoint.identity->recipient->public_key();
    }
    [[nodiscard]] bool matches(const IncomingContext &value, std::uint64_t time) const;
    void fail(IncomingListenerError error);
    void receive(Bytes wire);
    void retain(Bytes wire, std::uint64_t time);
    void prune_pending(std::uint64_t time);
    void retry(const IncomingContext &current, std::uint64_t time);
    void handle_result(IncomingOfferResult result);
    [[nodiscard]] MailboxCallbacks handlers();
};

bool NativeIncomingListener::State::matches(const IncomingContext &value,
                                            std::uint64_t time) const {
    return same_context(initial, value, time) &&
           value.local.endpoint.identity->signing->public_key() == signing_key &&
           value.local.endpoint.identity->recipient->public_key() == box_key;
}

void NativeIncomingListener::State::fail(IncomingListenerError error) {
    if (!active || !healthy)
        return;
    healthy = false;
    pending.clear();
    pending_bytes = 0;
    if (mailbox)
        mailbox->close();
    callbacks.on_error(error);
}

void NativeIncomingListener::State::handle_result(IncomingOfferResult result) {
    if (auto *offer = std::get_if<IncomingOffer>(&result)) {
        try {
            callbacks.on_offer(std::move(*offer));
        } catch (...) {
            fail(IncomingListenerError::Callback);
        }
    } else if (auto *error = std::get_if<IncomingOfferError>(&result)) {
        if (*error == IncomingOfferError::Full)
            fail(IncomingListenerError::ResourceLimit);
        if (*error == IncomingOfferError::ClockRegressed)
            fail(IncomingListenerError::Clock);
        if (*error == IncomingOfferError::ContextMismatch)
            fail(IncomingListenerError::Context);
    } else if (auto *error = std::get_if<CryptoError>(&result); error && fatal_crypto(*error)) {
        fail(IncomingListenerError::Crypto);
    }
}

void NativeIncomingListener::State::receive(Bytes wire) {
    if (!active || !healthy)
        return;
    try {
        if (callbacks.on_pending_record)
            callbacks.on_pending_record(wire);
        if (!active || !healthy)
            return;
        if (callbacks.can_offer && !callbacks.can_offer()) {
            return;
        }
        const auto time = now();
        auto current = context();
        if (!current || !matches(*current, time)) {
            fail(IncomingListenerError::Context);
            return;
        }
        const auto &local = current->local.endpoint;
        SignalingRecipient recipient{local.identity->signing->public_key(),
                                     local.fields.instance_id};
        auto result = resolver.resolve(wire, *local.identity->recipient, recipient,
                                       *current->local.capability, current->remote.peers, time);
        if (auto *error = std::get_if<IncomingOfferError>(&result);
            error && *error == IncomingOfferError::UnknownPeer)
            retain(std::move(wire), time);
        else
            handle_result(std::move(result));
    } catch (const std::bad_alloc &) {
        fail(IncomingListenerError::ResourceLimit);
    } catch (const std::exception &) {
        fail(IncomingListenerError::Crypto);
    }
}

void NativeIncomingListener::State::retain(Bytes wire, std::uint64_t time) {
    prune_pending(time);
    if (std::any_of(pending.begin(), pending.end(),
                    [&](const auto &item) { return item.wire == wire; }))
        return;
    const auto size = wire.capacity();
    if (pending.size() >= pending_limit || size > pending_bytes_limit - pending_bytes) {
        fail(IncomingListenerError::ResourceLimit);
        return;
    }
    auto reservation = budget.try_reserve(size);
    if (!reservation) {
        fail(IncomingListenerError::ResourceLimit);
        return;
    }
    pending.push_back({std::move(wire), std::move(*reservation), time});
    pending_bytes += size;
}

void NativeIncomingListener::State::prune_pending(std::uint64_t time) {
    const auto skew = initial.remote.clock_skew;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto retention = skew > (maximum - 60) / 2 ? maximum : 60 + 2 * skew;
    std::erase_if(pending, [&](const auto &entry) {
        if (time < entry.received || time - entry.received <= retention)
            return false;
        pending_bytes -= entry.reservation.bytes();
        return true;
    });
}

void NativeIncomingListener::State::retry(const IncomingContext &current, std::uint64_t time) {
    if (callbacks.can_offer && !callbacks.can_offer())
        return;
    prune_pending(time);
    if (pending.empty())
        return;
    auto entry = std::move(pending.front());
    pending.pop_front();
    pending_bytes -= entry.reservation.bytes();
    const auto &local = current.local.endpoint;
    SignalingRecipient recipient{local.identity->signing->public_key(), local.fields.instance_id};
    auto result = resolver.resolve(entry.wire, *local.identity->recipient, recipient,
                                   *current.local.capability, current.remote.peers, time);
    if (auto *error = std::get_if<IncomingOfferError>(&result);
        error && *error == IncomingOfferError::UnknownPeer) {
        pending_bytes += entry.reservation.bytes();
        pending.push_back(std::move(entry));
    } else
        handle_result(std::move(result));
}

MailboxCallbacks NativeIncomingListener::State::handlers() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                if (auto self = weak.lock())
                    self->receive(std::move(value));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock())
                    self->fail(resource_signal_error(error, detail)
                                   ? IncomingListenerError::ResourceLimit
                                   : IncomingListenerError::Mailbox);
            },
            [weak](bool ok) {
                if (auto self = weak.lock(); self && !ok)
                    self->fail(IncomingListenerError::Catchup);
            }};
}

NativeIncomingListener::NativeIncomingListener(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeIncomingListener::~NativeIncomingListener() { close(); }
std::variant<std::unique_ptr<NativeIncomingListener>, StoreError>
NativeIncomingListener::create(RecordStore store, IncomingContext initial,
                               std::function<std::optional<IncomingContext>()> context,
                               std::function<std::uint64_t()> now, Executor executor,
                               MemoryBudget budget, IncomingListenerCallbacks callbacks,
                               std::shared_ptr<IncomingOfferReplay> replay) {
    if (!context || !now || !executor || !callbacks.on_offer || !callbacks.on_error ||
        !initial.local.capability || initial.local.capability->size() != capability_size ||
        !initial.remote.max_lifetime || !same_context(initial, initial, now())) {
        throw std::invalid_argument("invalid incoming listener context");
    }
    auto state =
        std::make_shared<State>(freeze_context(std::move(initial)), std::move(context),
                                std::move(now), budget, std::move(callbacks), std::move(replay));
    auto mailbox = NativeMailbox::create(
        std::move(store), state->initial.local.endpoint.fields.signaling_inbox,
        {524288, 128, wire_limit}, std::move(budget), std::move(executor), state->handlers());
    if (auto *error = std::get_if<StoreError>(&mailbox))
        return *error;
    state->mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(mailbox));
    return std::unique_ptr<NativeIncomingListener>(new NativeIncomingListener(std::move(state)));
}

bool NativeIncomingListener::poll() {
    if (!is_open())
        return false;
    try {
        const auto time = state_->now();
        auto context = state_->context();
        if (!context || !state_->matches(*context, time)) {
            state_->fail(IncomingListenerError::Context);
            return false;
        }
        if (state_->resolver.prune(time)) {
            state_->fail(IncomingListenerError::Clock);
            return false;
        }
        state_->retry(*context, time);
        if (!state_->healthy)
            return false;
        if (!state_->catchup_started) {
            auto result = state_->mailbox->catch_up();
            if (std::holds_alternative<std::monostate>(result))
                state_->catchup_started = true;
            else if (auto *error = std::get_if<StoreError>(&result))
                state_->fail(resource_signal_error(std::optional{*error})
                                 ? IncomingListenerError::ResourceLimit
                                 : IncomingListenerError::Catchup);
            else if (std::get<CatchupError>(result) != CatchupError::RegistrationPending)
                state_->fail(IncomingListenerError::Catchup);
        }
    } catch (const std::bad_alloc &) {
        state_->fail(IncomingListenerError::ResourceLimit);
    } catch (const std::exception &) {
        state_->fail(IncomingListenerError::Clock);
    }
    return is_open();
}
bool NativeIncomingListener::is_open() const {
    return state_->active && state_->healthy && state_->mailbox && state_->mailbox->is_open();
}
std::size_t NativeIncomingListener::pending_offers() const { return state_->pending.size(); }
void NativeIncomingListener::close() {
    state_->active = false;
    state_->pending.clear();
    state_->pending_bytes = 0;
    if (state_->mailbox)
        state_->mailbox->close();
}
} // namespace p2pdc::detail
