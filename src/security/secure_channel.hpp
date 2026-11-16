#pragma once

#include "async_signaling.hpp"
#include "handshake_gate.hpp"

namespace p2pdc::detail {

enum class SecureFailure { Protocol, Transport, Resource, Timeout };
struct SecureChannelCallbacks {
    std::function<void(std::uint32_t)> on_ready;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(SecureFailure)> on_failure;
};

// Internal adapter: signaling must belong to this transport attempt and its
// exact installed SDP, with fingerprint checking enabled. Identity, sink and
// executor and signaling outlive this channel. All operations and callbacks are serialized;
// sink, executor and application callbacks must obey the underlying nonthrowing,
// non-reentrant contracts and must not destroy this channel during callbacks.
// The owner closes the underlying network transport on failure and drives the
// handshake deadline using timeout(). An incoming frame before start fails closed.
class SecureChannel final {
  public:
    SecureChannel(const AsyncSignaling &signaling, const Identity &identity, HandshakeRole role,
                  std::uint32_t receive_limit, ReliableChannelLimits limits, MemoryBudget budget,
                  ReliableSink &sink, Executor executor, SecureChannelCallbacks callbacks);
    ~SecureChannel();
    SecureChannel(const SecureChannel &) = delete;
    SecureChannel &operator=(const SecureChannel &) = delete;
    [[nodiscard]] bool start();
    [[nodiscard]] ChannelSend send(ByteView message);
    void set_transport_ready(bool ready);
    void on_frame(ByteView frame);
    void wake();
    void timeout();
    void close();
    [[nodiscard]] bool ready() const;

  private:
    class Transport final : public HandshakeTransport {
      public:
        explicit Transport(ReliableChannel &channel) : channel_(channel) {}
        ChannelSend send(ByteView message) override { return channel_.send(message); }
        void limit_receive(std::size_t bytes) override { channel_.set_receive_limit(bytes); }
        void close() override { channel_.close(); }

      private:
        ReliableChannel &channel_;
    };
    void fail(SecureFailure reason);
    [[nodiscard]] ReliableChannelCallbacks channel_callbacks();
    [[nodiscard]] HandshakeGateCallbacks gate_callbacks();
    const AsyncSignaling &signaling_;
    const Identity &identity_;
    HandshakeRole role_;
    std::uint32_t receive_limit_;
    std::size_t transport_limit_;
    SecureChannelCallbacks callbacks_;
    ReliableChannel channel_;
    Transport transport_;
    std::unique_ptr<HandshakeGate> gate_;
    bool transport_ready_ = false;
    bool stopped_ = false;
};

} // namespace p2pdc::detail
