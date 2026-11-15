#pragma once

#include "secure_channel.hpp"

namespace p2pdc::detail {

enum class AttemptFailure { Signaling, Protocol, Transport, Resource, Timeout, Rejected, Canceled };
struct SecureAttemptCallbacks {
    SignalingFlowCallbacks signaling;
    std::function<void(std::uint32_t)> on_ready;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(AttemptFailure)> on_failure;
};

// One preauthorized connection attempt, binding signaling and channel together.
// Exact installed local SDP, fingerprint verification, signaling authentication
// inputs and backend ownership are caller responsibilities. Identity, recipient
// key, sink and executor outlive this object. Operations and callbacks are
// serialized and non-reentrant; callbacks do not throw or destroy this object.
// The owner must close its underlying network transport on failure and enforce
// deadlines via timeout(). Application sends and messages pass through the gate.
class SecureAttempt final {
  public:
    SecureAttempt(AsyncSignalingBinding binding, ByteView capability, const Identity &identity,
                  const RecipientKeyPair &local_box, CandidateLimits candidate_limits,
                  MemoryBudget budget, ReliableChannelLimits channel_limits,
                  std::uint32_t receive_limit, ReliableSink &sink, Executor executor,
                  SecureAttemptCallbacks callbacks);
    ~SecureAttempt();
    SecureAttempt(const SecureAttempt &) = delete;
    SecureAttempt &operator=(const SecureAttempt &) = delete;
    [[nodiscard]] FlowResult local_description(ByteView installed_sdp, std::uint64_t issued,
                                               std::uint64_t expiry, SignalingPolicy policy);
    [[nodiscard]] FlowResult local_candidate(CandidateView candidate, std::uint64_t issued,
                                             std::uint64_t expiry, SignalingPolicy policy);
    [[nodiscard]] FlowResult receive_signaling(ByteView wire, SignalingPolicy policy);
    [[nodiscard]] FlowResult description_applied(bool success);
    [[nodiscard]] FlowResult candidate_applied(std::uint64_t operation, bool success);
    [[nodiscard]] std::optional<std::uint64_t> candidate_operation() const;
    [[nodiscard]] ChannelSend send(ByteView message);
    void set_transport_ready(bool ready);
    void on_frame(ByteView frame);
    void wake();
    void timeout();
    void close();
    [[nodiscard]] bool ready() const;

  private:
    struct Secret {
        explicit Secret(ByteView value);
        ~Secret();
        Secret(const Secret &) = delete;
        Secret &operator=(const Secret &) = delete;
        std::array<std::byte, capability_size> bytes{};
    };
    [[nodiscard]] FlowResult track(FlowResult result);
    void maybe_start();
    void fail(AttemptFailure reason);
    [[nodiscard]] SignalingFlowCallbacks signaling_callbacks();
    [[nodiscard]] SecureChannelCallbacks channel_callbacks();
    const Identity &identity_;
    const RecipientKeyPair &local_box_;
    Secret capability_;
    SecureAttemptCallbacks callbacks_;
    AsyncSignaling signaling_;
    SecureChannel channel_;
    bool started_ = false;
    bool stopped_ = false;
    AttemptFailure signaling_failure_ = AttemptFailure::Signaling;
};

} // namespace p2pdc::detail
