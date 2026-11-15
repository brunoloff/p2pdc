#pragma once

#include "../discovery/native_signal_pipe.hpp"
#include "../discovery/native_store.hpp"
#include "../security/handshake_gate.hpp"
#include "native_signaling.hpp"
#include "p2pdc/ice_config.h"
#include "public_attempt.hpp"

namespace p2pdc::detail {

using NativeAttemptState = ConnectionAttemptState;
using NativeStart = ConnectionAttemptStart;

struct NativeAttemptConfig {
    NativeStore store;
    std::array<std::byte, 20> local_mailbox;
    std::array<std::byte, 20> remote_mailbox;
    NativeSignalBinding binding;
    std::shared_ptr<const Identity> identity;
    std::shared_ptr<const RecipientKeyPair> recipient_box;
    std::shared_ptr<const Bytes> capability;
    IceConfig ice;
    MemoryBudget budget;
    SignalPipeLimits pipe_limits;
    ReliableChannelLimits channel_limits;
    NativeReceiveLimits receive_limits;
    CandidateLimits candidate_limits;
    std::size_t candidate_bytes;
    std::size_t candidate_count;
    std::uint32_t application_limit;
    std::size_t gate_transport_limit;
    SignalingPolicy signaling_policy;
    std::uint64_t signaling_ttl;
    std::function<std::uint64_t()> now;
    // A responder may pin the exact offer approved by its incoming dispatcher.
    std::optional<Bytes> accepted_offer = std::nullopt;
};

using NativeAttemptCallbacks = ConnectionAttemptCallbacks;

// Internal preauthorized single attempt, not a discovery or acceptance API.
// The attempt retains the identity, box and capability; no mutable aliases may
// change them during its lifetime. The DHT runner and
// executor outlive the attempt and pending backend work. Methods and callbacks use one serialized
// executor; callbacks must not reenter or destroy the attempt. poll_start is
// nonblocking. The owner calls timeout if its deadline expires and polls health
// and route regularly. poll_health also wakes writes blocked on shared budget.
// close cannot retract records already published to DHT.
class NativeAttempt : public PublicAttempt {
  public:
    static std::variant<std::unique_ptr<NativeAttempt>, StoreError>
    create(NativeAttemptConfig config, Executor executor, NativeAttemptCallbacks callbacks);
    ~NativeAttempt() override;
    NativeAttempt(const NativeAttempt &) = delete;
    NativeAttempt &operator=(const NativeAttempt &) = delete;
    [[nodiscard]] NativeStart poll_start() override;
    [[nodiscard]] ChannelSend send(ByteView payload) override;
    [[nodiscard]] NativeAttemptState state() const override;
    [[nodiscard]] ConnectError failure_reason() const override;
    [[nodiscard]] std::optional<std::uint32_t> max_message_size() const override;
    [[nodiscard]] bool signaling_unavailable() const override;
    // Delivers an outbox failure on the attempt executor. Also used by the
    // isolated transport fixture to exercise backend failures deterministically.
    void on_signaling_error(OutboxError error);
    void on_signaling_catchup(bool success);
    void poll_health() override;
    [[nodiscard]] RouteInfo poll_route() override;
    void timeout();
    void close() override;
    [[nodiscard]] NativeAttempt *native_for_testing() override { return this; }

  private:
    struct Impl;
    explicit NativeAttempt(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace p2pdc::detail
