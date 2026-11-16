#include "secure_channel.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
ReliableChannelLimits checked_limits(ReliableChannelLimits limits, std::uint32_t receive_limit,
                                     const SecureChannelCallbacks &callbacks) {
    if (!callbacks.on_ready || !callbacks.on_message || !callbacks.on_writable ||
        !callbacks.on_failure || receive_limit == 0 ||
        limits.frames.max_message_size < 1 + handshake_record_size ||
        receive_limit > limits.frames.max_message_size - 1)
        throw std::invalid_argument("invalid secure channel limits or callbacks");
    return limits;
}
SecureFailure map_failure(ChannelFailure reason) {
    switch (reason) {
    case ChannelFailure::Transport:
        return SecureFailure::Transport;
    case ChannelFailure::InvalidFrame:
        return SecureFailure::Protocol;
    default:
        return SecureFailure::Resource;
    }
}
SecureFailure map_failure(GateError reason) {
    switch (reason) {
    case GateError::Protocol:
        return SecureFailure::Protocol;
    case GateError::Transport:
        return SecureFailure::Transport;
    case GateError::Timeout:
        return SecureFailure::Timeout;
    default:
        return SecureFailure::Resource;
    }
}
} // namespace

SecureChannel::SecureChannel(const AsyncSignaling &signaling, const Identity &identity,
                             HandshakeRole role, std::uint32_t receive_limit,
                             ReliableChannelLimits limits, MemoryBudget budget, ReliableSink &sink,
                             Executor executor, SecureChannelCallbacks callbacks)
    : signaling_(signaling), identity_(identity), role_(role), receive_limit_(receive_limit),
      transport_limit_(limits.frames.max_message_size), callbacks_(std::move(callbacks)),
      channel_(checked_limits(limits, receive_limit, callbacks_), std::move(budget), sink,
               std::move(executor), channel_callbacks()),
      transport_(channel_) {
    if (role != HandshakeRole::Offerer && role != HandshakeRole::Answerer)
        throw std::invalid_argument("invalid handshake role");
    channel_.set_receive_limit(1 + handshake_record_size);
}
SecureChannel::~SecureChannel() { close(); }

ReliableChannelCallbacks SecureChannel::channel_callbacks() {
    return {[this](Bytes message) {
                if (!stopped_ && gate_)
                    gate_->on_message(std::move(message));
            },
            [this](ChannelFailure reason) { fail(map_failure(reason)); },
            [this] {
                if (!stopped_ && gate_)
                    gate_->on_writable();
            }};
}
HandshakeGateCallbacks SecureChannel::gate_callbacks() {
    return {[this](std::uint32_t limit) {
                if (!stopped_)
                    callbacks_.on_ready(limit);
            },
            [this](Bytes message) {
                if (!stopped_)
                    callbacks_.on_message(std::move(message));
            },
            [this] {
                if (!stopped_)
                    callbacks_.on_writable();
            },
            [this](GateError reason) { fail(map_failure(reason)); }};
}
bool SecureChannel::start() {
    if (stopped_ || gate_)
        return false;
    auto context = signaling_.handshake_context();
    if (!context)
        return false;
    const auto expected =
        role_ == HandshakeRole::Offerer ? context->offerer_key : context->answerer_key;
    if (identity_.public_key() != expected) {
        fail(SecureFailure::Protocol);
        return false;
    }
    try {
        gate_ = std::make_unique<HandshakeGate>(identity_, *context, role_, receive_limit_,
                                                transport_limit_, transport_, gate_callbacks());
        gate_->set_transport_ready(transport_ready_);
        return !stopped_;
    } catch (const std::exception &) {
        fail(SecureFailure::Resource);
        return false;
    }
}
ChannelSend SecureChannel::send(ByteView message) {
    if (stopped_)
        return ChannelSend::Closed;
    return gate_ ? gate_->send(message) : ChannelSend::NotReady;
}
void SecureChannel::set_transport_ready(bool ready) {
    if (stopped_)
        return;
    transport_ready_ = ready;
    if (!ready && gate_)
        gate_->set_transport_ready(false);
    channel_.set_ready(ready);
    if (ready && gate_)
        gate_->set_transport_ready(true);
}
void SecureChannel::on_frame(ByteView frame) {
    if (stopped_)
        return;
    // start() runs as soon as both descriptions are installed, which precedes
    // any data-channel traffic (ICE and DTLS need them), so an honest peer's
    // frame cannot arrive before the gate exists.
    if (!gate_) {
        fail(SecureFailure::Protocol);
        return;
    }
    channel_.on_frame(frame);
}
void SecureChannel::wake() {
    if (!stopped_)
        channel_.wake();
}
void SecureChannel::timeout() {
    if (stopped_)
        return;
    if (gate_)
        gate_->timeout();
    else
        fail(SecureFailure::Timeout);
}
bool SecureChannel::ready() const { return !stopped_ && gate_ && gate_->ready(); }
void SecureChannel::fail(SecureFailure reason) {
    if (stopped_)
        return;
    stopped_ = true;
    if (gate_)
        gate_->close();
    channel_.close();
    callbacks_.on_failure(reason);
}
void SecureChannel::close() {
    if (stopped_)
        return;
    stopped_ = true;
    if (gate_)
        gate_->close();
    channel_.close();
}
} // namespace p2pdc::detail
