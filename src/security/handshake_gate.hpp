#pragma once

#include "../protocol/reliable_channel.hpp"
#include "p2pdc/handshake_exchange.h"

namespace p2pdc::detail {

// Transport must deliver accepted messages in order, never reenter a gate method
// during send/close, and remain alive until the gate is destroyed.
class HandshakeTransport {
  public:
    virtual ~HandshakeTransport() = default;
    [[nodiscard]] virtual ChannelSend send(ByteView message) = 0;
    // Caps the size of messages the transport will start receiving. The gate
    // sets it to one control record until the handshake completes, then to the
    // transport message limit.
    virtual void limit_receive(std::size_t bytes) = 0;
    virtual void close() = 0;
};

enum class GateError { Protocol, Transport, Allocation, Timeout };
struct HandshakeGateCallbacks {
    std::function<void(std::uint32_t)> on_ready;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(GateError)> on_error;
};

// Internal gate. Context MUST come from authorized, verified signaling and hash
// the exact offer/answer installed in the transport with fingerprint checks on.
// It does not verify that prerequisite. Identity and transport outlive the gate.
// Methods and callbacks share one serialized context; backend events are already
// posted asynchronously. User callbacks and transport operations do not throw.
// Caller enforces the handshake deadline and calls timeout() when it expires.
// Owned send buffers temporarily duplicate payload bytes outside budget accounting.
// The optional before_control_verify test hook may throw bad_alloc or length_error.
class HandshakeGate {
  public:
    HandshakeGate(const Identity &identity, HandshakeContext context, HandshakeRole role,
                  std::uint32_t receive_limit, std::size_t transport_message_limit,
                  HandshakeTransport &transport, HandshakeGateCallbacks callbacks,
                  std::function<void()> before_control_verify = {});
    ~HandshakeGate();
    HandshakeGate(const HandshakeGate &) = delete;
    HandshakeGate &operator=(const HandshakeGate &) = delete;
    [[nodiscard]] ChannelSend send(ByteView message);
    void set_transport_ready(bool ready);
    // Takes ownership so application bytes are delivered without a copy.
    void on_message(Bytes message);
    void on_writable();
    void timeout();
    void close();
    [[nodiscard]] bool ready() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace p2pdc::detail
