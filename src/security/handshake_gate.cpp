#include "handshake_gate.hpp"

#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::byte control{0};
constexpr std::byte application{1};

HandshakeNonce fresh_nonce() {
    auto nonce = generate_handshake_nonce();
    if (auto *value = std::get_if<HandshakeNonce>(&nonce)) {
        return *value;
    }
    throw std::runtime_error("cannot generate handshake challenge");
}
} // namespace

struct HandshakeGate::Impl : std::enable_shared_from_this<Impl> {
    HandshakeExchange exchange;
    HandshakeTransport &transport;
    HandshakeGateCallbacks callbacks;
    std::function<void()> before_control_verify;
    std::optional<Bytes> pending;
    bool transport_ready = false;
    bool hello_sent = false;
    bool confirmation_sent = false;
    bool activated = false;
    bool stopped = false;

    Impl(const Identity &identity, HandshakeContext context, HandshakeRole role,
         std::uint32_t limit, HandshakeTransport &link, HandshakeGateCallbacks handlers,
         std::function<void()> before_verify)
        : exchange(identity, std::move(context), role, fresh_nonce(), limit), transport(link),
          callbacks(std::move(handlers)), before_control_verify(std::move(before_verify)) {}

    void stop() {
        stopped = true;
        pending.reset();
        transport.close();
    }
    void fail(GateError error) {
        if (!stopped) {
            stop();
            callbacks.on_error(error);
        }
    }

    bool prepare_control() {
        if (pending) {
            return true;
        }
        auto encoded = hello_sent ? exchange.confirmation() : exchange.hello();
        if (std::holds_alternative<HandshakeError>(encoded)) {
            if (hello_sent && std::get<HandshakeError>(encoded) == HandshakeError::InvalidState) {
                return false; // Waiting for the peer hello.
            }
            fail(GateError::Protocol);
            return false;
        }
        if (std::holds_alternative<CryptoError>(encoded)) {
            fail(GateError::Protocol);
            return false;
        }
        pending.emplace();
        pending->reserve(1 + handshake_record_size);
        pending->push_back(control);
        const auto &wire = std::get<Bytes>(encoded);
        pending->insert(pending->end(), wire.begin(), wire.end());
        return true;
    }

    bool send_control() {
        const auto result = transport.send(*pending);
        if (result == ChannelSend::NotReady || result == ChannelSend::Backpressure) {
            return false;
        }
        if (result != ChannelSend::Accepted) {
            fail(GateError::Transport);
            return false;
        }
        pending.reset();
        auto marked = hello_sent ? exchange.mark_confirmation_sent() : exchange.mark_hello_sent();
        if (!std::holds_alternative<std::monostate>(marked)) {
            fail(GateError::Protocol);
            return false;
        }
        if (hello_sent) {
            confirmation_sent = true;
        } else {
            hello_sent = true;
        }
        return true;
    }

    void drive() {
        if (stopped || !transport_ready || activated) {
            return;
        }
        try {
            while (!confirmation_sent && prepare_control() && send_control()) {
            }
            if (!stopped && exchange.ready()) {
                activated = true;
                callbacks.on_ready(*exchange.message_limit());
            }
        } catch (const std::bad_alloc &) {
            fail(GateError::Allocation);
        } catch (const std::length_error &) {
            fail(GateError::Allocation);
        }
    }

    void receive(ByteView bytes) {
        if (stopped) {
            return;
        }
        try {
            receive_checked(bytes);
        } catch (const std::bad_alloc &) {
            fail(GateError::Allocation);
        } catch (const std::length_error &) {
            fail(GateError::Allocation);
        }
    }

    void receive_checked(ByteView bytes) {
        if (!transport_ready || bytes.empty()) {
            fail(GateError::Protocol);
        } else if (bytes.front() == control && !activated) {
            if (before_control_verify) {
                before_control_verify();
            }
            auto result = exchange.receive(bytes.subspan(1));
            if (!std::holds_alternative<std::monostate>(result)) {
                fail(GateError::Protocol);
            } else {
                drive();
            }
        } else if (bytes.front() == application && activated &&
                   bytes.size() - 1 <= *exchange.message_limit()) {
            callbacks.on_message(Bytes(bytes.begin() + 1, bytes.end()));
        } else {
            fail(GateError::Protocol);
        }
    }

    ChannelSend send(ByteView message) {
        if (stopped) {
            return ChannelSend::Closed;
        }
        if (!activated || !transport_ready) {
            return ChannelSend::NotReady;
        }
        if (message.size() > *exchange.message_limit()) {
            return ChannelSend::TooLarge;
        }
        try {
            Bytes frame;
            frame.reserve(message.size() + 1);
            frame.push_back(application);
            frame.insert(frame.end(), message.begin(), message.end());
            return transport.send(frame);
        } catch (const std::bad_alloc &) {
            return ChannelSend::Failed;
        } catch (const std::length_error &) {
            return ChannelSend::Failed;
        }
    }
};

HandshakeGate::HandshakeGate(const Identity &identity, HandshakeContext context, HandshakeRole role,
                             std::uint32_t receive_limit, std::size_t transport_message_limit,
                             HandshakeTransport &transport, HandshakeGateCallbacks callbacks,
                             std::function<void()> before_control_verify) {
    if (!callbacks.on_ready || !callbacks.on_message || !callbacks.on_writable ||
        !callbacks.on_error || transport_message_limit < 1 + handshake_record_size ||
        receive_limit > transport_message_limit - 1) {
        throw std::invalid_argument("invalid handshake gate limits or callbacks");
    }
    impl_ = std::make_shared<Impl>(identity, std::move(context), role, receive_limit, transport,
                                   std::move(callbacks), std::move(before_control_verify));
}
HandshakeGate::~HandshakeGate() { close(); }
ChannelSend HandshakeGate::send(ByteView message) { return impl_->send(message); }
void HandshakeGate::set_transport_ready(bool ready) {
    auto self = impl_;
    if (!self->stopped) {
        self->transport_ready = ready;
        self->drive();
    }
}
void HandshakeGate::on_message(ByteView message) {
    auto self = impl_;
    self->receive(message);
}
void HandshakeGate::on_writable() {
    auto self = impl_;
    if (!self->stopped && self->transport_ready) {
        if (self->activated) {
            self->callbacks.on_writable();
        } else {
            self->drive();
        }
    }
}
void HandshakeGate::timeout() {
    auto self = impl_;
    if (!self->activated) {
        self->fail(GateError::Timeout);
    }
}
void HandshakeGate::close() {
    if (impl_ && !impl_->stopped) {
        impl_->stop();
    }
}
bool HandshakeGate::ready() const {
    return impl_->activated && impl_->transport_ready && !impl_->stopped;
}

} // namespace p2pdc::detail
