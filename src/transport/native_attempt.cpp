#include "../discovery/signal_errors.hpp"
#include "native_attempt_impl.hpp"
#include "native_attempt_validation.hpp"
#include "native_ice_config.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
ConnectError gate_error(GateError error) {
    switch (error) {
    case GateError::Protocol:
        return ConnectError::Protocol;
    case GateError::Transport:
        return ConnectError::Transport;
    case GateError::Allocation:
        return ConnectError::ResourceLimit;
    case GateError::Timeout:
        return ConnectError::Timeout;
    }
    return ConnectError::Backend;
}
} // namespace

SignalingPolicy NativeAttempt::Impl::policy() const {
    auto result = config.signaling_policy;
    result.now = config.now();
    return result;
}
std::uint64_t NativeAttempt::Impl::expiry(std::uint64_t now) const {
    if (config.signaling_ttl > std::numeric_limits<std::uint64_t>::max() - now) {
        throw std::overflow_error("signaling expiry overflow");
    }
    return now + config.signaling_ttl;
}

void NativeAttempt::Impl::fail(ConnectError reason) {
    if (status == NativeAttemptState::Failed || status == NativeAttemptState::Closed)
        return;
    status = NativeAttemptState::Failed;
    failure_reason = reason;
    executor([weak = weak_from_this()] {
        if (auto self = weak.lock(); self && self->status == NativeAttemptState::Failed) {
            self->finish_failure();
        }
    });
}
void NativeAttempt::Impl::finish_failure() {
    stop();
    callbacks.on_failed();
}
void NativeAttempt::Impl::stop() {
    if (cleaned)
        return;
    cleaned = true;
    if (pipe)
        pipe->close();
    if (signaling)
        signaling->close();
    if (gate)
        gate->close();
    if (link)
        link->close();
}

void NativeAttempt::Impl::on_description(rtc::Description value) {
    if (signaling_unavailable || status == NativeAttemptState::Failed ||
        status == NativeAttemptState::Closed)
        return;
    try {
        auto current = policy();
        if (signaling->local_description(*config.identity, *config.capability, std::move(value),
                                         current.now, expiry(current.now),
                                         current) != FlowResult::Accepted) {
            fail(ConnectError::Signaling);
            return;
        }
        activate();
    } catch (const std::exception &) {
        fail();
    }
}
void NativeAttempt::Impl::on_candidate(rtc::Candidate value) {
    if (signaling_unavailable || status == NativeAttemptState::Failed ||
        status == NativeAttemptState::Closed)
        return;
    try {
        auto current = policy();
        if (signaling->local_candidate(*config.identity, *config.capability, std::move(value),
                                       current.now, expiry(current.now),
                                       current) != FlowResult::Accepted)
            fail(ConnectError::Signaling);
    } catch (const std::exception &) {
        fail();
    }
}
void NativeAttempt::Impl::on_record(Bytes value) {
    if (!started || signaling_unavailable || status == NativeAttemptState::Failed ||
        status == NativeAttemptState::Closed)
        return;
    try {
        auto result =
            signaling->receive(value, *config.recipient_box, *config.capability, policy());
        if (result == FlowResult::Failed || result == FlowResult::Closed)
            fail();
        else if (result == FlowResult::Accepted)
            activate();
    } catch (const std::exception &) {
        fail();
    }
}

bool NativeAttempt::Impl::apply_accepted_offer() {
    if (!config.accepted_offer)
        return true;
    const auto current = policy();
    auto decoded = decode_signaling(*config.accepted_offer, *config.recipient_box,
                                    config.binding.local, *config.capability, current);
    const auto *envelope = std::get_if<SignalingEnvelope>(&decoded);
    if (!envelope || envelope->fields.kind != SignalingKind::Offer) {
        config.accepted_offer.reset();
        fail();
        return false;
    }
    const auto result = signaling->receive(*config.accepted_offer, *config.recipient_box,
                                           *config.capability, current);
    config.accepted_offer.reset();
    if (result != FlowResult::Accepted) {
        fail();
        return false;
    }
    activate();
    return status != NativeAttemptState::Failed;
}

void NativeAttempt::Impl::activate() {
    if (!transport_ready || status == NativeAttemptState::Failed ||
        status == NativeAttemptState::Closed)
        return;
    if (!gate) {
        auto context = signaling->handshake_context();
        if (!context)
            return;
        try {
            gate = std::make_unique<HandshakeGate>(
                config.identity, *context, config.binding.role, config.application_limit,
                config.gate_transport_limit, *this, gate_callbacks());
            status = NativeAttemptState::Authenticating;
        } catch (const std::exception &) {
            fail();
            return;
        }
    }
    gate->set_transport_ready(true);
    if (gate->ready() && status == NativeAttemptState::Suspended) {
        status = NativeAttemptState::Ready;
    }
}
void NativeAttempt::Impl::on_transport(NativeTransportState value) {
    if (status == NativeAttemptState::Failed || status == NativeAttemptState::Closed)
        return;
    if (value == NativeTransportState::Ready) {
        transport_ready = true;
        activate();
    } else if (value == NativeTransportState::Suspended) {
        transport_ready = false;
        if (gate)
            gate->set_transport_ready(false);
        status = NativeAttemptState::Suspended;
    } else if (value == NativeTransportState::Failed || value == NativeTransportState::Closed) {
        fail(ConnectError::Transport);
    }
}

NativeLinkCallbacks NativeAttempt::Impl::link_callbacks() {
    auto weak = weak_from_this();
    NativeLinkCallbacks out;
    out.on_description = [weak](rtc::Description value) {
        if (auto self = weak.lock())
            self->on_description(std::move(value));
    };
    out.on_candidate = [weak](rtc::Candidate value) {
        if (auto self = weak.lock())
            self->on_candidate(std::move(value));
    };
    out.on_state = [weak](NativeTransportState value) {
        if (auto self = weak.lock())
            self->on_transport(value);
    };
    out.on_route = [weak](RouteInfo value) {
        if (auto self = weak.lock(); self && self->authenticated &&
                                     (self->status == NativeAttemptState::Ready ||
                                      self->status == NativeAttemptState::Suspended))
            self->callbacks.on_route(value);
    };
    out.on_message = [weak](Bytes value) {
        if (auto self = weak.lock(); self && self->status != NativeAttemptState::Failed &&
                                     self->status != NativeAttemptState::Closed) {
            // Messages are delivered only after the Ready notification, which
            // creates the gate; the handshake context exists by then because
            // the local description task precedes readiness on the FIFO
            // executor. A message without a gate is a protocol violation.
            if (self->gate)
                self->gate->on_message(std::move(value));
            else
                self->fail();
        }
    };
    out.on_error = [weak](ConnectError category, std::string) {
        if (auto self = weak.lock())
            self->fail(category);
    };
    out.on_writable = [weak] {
        if (auto self = weak.lock(); self && self->gate &&
                                     self->status != NativeAttemptState::Failed &&
                                     self->status != NativeAttemptState::Closed)
            self->gate->on_writable();
    };
    out.on_datagram = [weak](Bytes value) {
        if (auto self = weak.lock())
            self->on_datagram(std::move(value));
    };
    out.can_receive = callbacks.can_receive;
    return out;
}
HandshakeGateCallbacks NativeAttempt::Impl::gate_callbacks() {
    auto weak = weak_from_this();
    return {[weak](std::uint32_t limit) {
                if (auto self = weak.lock(); self && self->status != NativeAttemptState::Failed &&
                                             self->status != NativeAttemptState::Closed) {
                    self->status = NativeAttemptState::Ready;
                    self->authenticated = true;
                    self->message_limit = limit;
                    self->callbacks.on_ready();
                    (void)self->link->poll_route();
                }
            },
            [weak](Bytes value) {
                if (auto self = weak.lock(); self && self->status == NativeAttemptState::Ready)
                    self->callbacks.on_message(std::move(value));
            },
            [weak] {
                if (auto self = weak.lock(); self && self->status == NativeAttemptState::Ready)
                    self->callbacks.on_writable();
            },
            [weak](GateError error) {
                if (auto self = weak.lock())
                    self->fail(gate_error(error));
            }};
}

std::optional<StoreError> NativeAttempt::Impl::setup() {
    auto ice = make_native_ice_config(config.ice);
    link = std::make_unique<NativeReliableLink>(std::move(ice.rtc), ice.policy,
                                                config.channel_limits, config.budget,
                                                config.receive_limits, executor, link_callbacks());
    // Until the handshake gate exists and completes, only control records may arrive.
    link->set_receive_limit(1 + handshake_record_size);
    auto result =
        NativeSignalPipe::create(config.store, config.local_mailbox, config.remote_mailbox,
                                 config.budget, config.pipe_limits, executor, pipe_callbacks());
    if (auto *error = std::get_if<StoreError>(&result))
        return *error;
    pipe = std::move(std::get<std::unique_ptr<NativeSignalPipe>>(result));
    auto weak = weak_from_this();
    signaling = std::make_unique<NativeSignaling>(
        config.binding, *config.capability, *link, config.candidate_limits, config.budget,
        config.candidate_bytes, config.candidate_count,
        [weak](Bytes value) {
            if (auto self = weak.lock(); self && self->pipe)
                return self->pipe->send(std::move(value)) == SignalingQueueResult::Accepted;
            return false;
        },
        [weak](SignalingKind kind) {
            if (auto self = weak.lock())
                self->fail(kind == SignalingKind::Reject ? ConnectError::Rejected
                                                         : ConnectError::Canceled);
            return true;
        },
        config.datagrams);
    return std::nullopt;
}

NativeAttempt::NativeAttempt(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeAttempt::~NativeAttempt() { close(); }
std::variant<std::unique_ptr<NativeAttempt>, StoreError>
NativeAttempt::create(NativeAttemptConfig config, Executor executor,
                      NativeAttemptCallbacks callbacks) {
    if (!executor || !config.now || !config.identity || !config.recipient_box ||
        !config.capability || config.capability->size() != capability_size || !callbacks.on_ready ||
        !callbacks.on_message || !callbacks.on_writable || !callbacks.on_route ||
        !callbacks.on_failed || config.signaling_ttl == 0 ||
        config.signaling_ttl > config.signaling_policy.max_lifetime || !valid_gate_limits(config) ||
        !valid_accepted_offer(config) ||
        config.identity->public_key() != config.binding.local.signing_key ||
        config.recipient_box->public_key() != config.binding.local_box) {
        throw std::invalid_argument("invalid native attempt configuration");
    }
    auto impl =
        std::make_shared<Impl>(std::move(config), std::move(executor), std::move(callbacks));
    if (auto error = impl->setup())
        return *error;
    return std::unique_ptr<NativeAttempt>(new NativeAttempt(std::move(impl)));
}
NativeStart NativeAttempt::poll_start() {
    auto impl = impl_;
    if (impl->status == NativeAttemptState::Closed)
        return NativeStart::Closed;
    if (impl->status == NativeAttemptState::Failed)
        return NativeStart::Failed;
    if (impl->started)
        return NativeStart::AlreadyStarted;
    if (!impl->pipe->is_open()) {
        impl->fail(ConnectError::Signaling);
        return NativeStart::Failed;
    }
    auto registration = impl->pipe->registration();
    if (registration.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return NativeStart::Pending;
    try {
        if (registration.get() == 0) {
            impl->fail(ConnectError::Signaling);
            return NativeStart::Failed;
        }
        impl->started = true;
        impl->status = NativeAttemptState::Connecting;
        if (!impl->apply_accepted_offer())
            return NativeStart::Failed;
        auto catchup = impl->pipe->catch_up();
        if (!std::holds_alternative<std::monostate>(catchup)) {
            impl->fail(resource_catchup_error(catchup) ? ConnectError::ResourceLimit
                                                       : ConnectError::Signaling);
            return NativeStart::Failed;
        }
        if (impl->config.binding.role == HandshakeRole::Offerer)
            impl->link->start_offer();
        return NativeStart::Started;
    } catch (const std::exception &) {
        impl->fail();
        return NativeStart::Failed;
    }
}
ChannelSend NativeAttempt::send(ByteView payload) {
    if (impl_->status == NativeAttemptState::Closed)
        return ChannelSend::Closed;
    if (impl_->status == NativeAttemptState::Failed)
        return ChannelSend::Failed;
    if (impl_->status != NativeAttemptState::Ready || !impl_->gate)
        return ChannelSend::NotReady;
    return impl_->gate->send(payload);
}
NativeAttemptState NativeAttempt::state() const { return impl_->status; }
ConnectError NativeAttempt::failure_reason() const { return impl_->failure_reason; }
bool NativeAttempt::signaling_unavailable() const { return impl_->signaling_unavailable; }
std::optional<std::uint32_t> NativeAttempt::max_message_size() const {
    if (impl_->status == NativeAttemptState::Failed || impl_->status == NativeAttemptState::Closed)
        return std::nullopt;
    return impl_->message_limit;
}
void NativeAttempt::resume_receive() {
    if (impl_->link && impl_->status != NativeAttemptState::Closed &&
        impl_->status != NativeAttemptState::Failed)
        impl_->link->resume_receive();
}
void NativeAttempt::poll_health() {
    if (!impl_->started || impl_->status == NativeAttemptState::Closed ||
        impl_->status == NativeAttemptState::Failed)
        return;
    if (!impl_->signaling_unavailable && !impl_->pipe->is_open())
        impl_->fail(ConnectError::Signaling);
    else {
        if (!impl_->signaling_unavailable)
            impl_->pipe->retry_budget();
        impl_->link->wake();
    }
}
RouteInfo NativeAttempt::poll_route() { return impl_->link->poll_route(); }
void NativeAttempt::timeout() { impl_->fail(ConnectError::Timeout); }
void NativeAttempt::close() {
    if (impl_ && impl_->status != NativeAttemptState::Closed) {
        if ((impl_->started || impl_->config.accepted_offer) && !impl_->authenticated &&
            impl_->status != NativeAttemptState::Failed)
            impl_->cancel_remote();
        impl_->status = NativeAttemptState::Closed;
        impl_->stop();
    }
}

} // namespace p2pdc::detail
