#pragma once

#include "../protocol/reliable_channel.hpp"
#include "native_transport.hpp"

namespace p2pdc::detail {

[[nodiscard]] ConnectError channel_failure_category(ChannelFailure failure);

struct NativeLinkCallbacks {
    std::function<void(rtc::Description)> on_description;
    std::function<void(rtc::Candidate)> on_candidate;
    std::function<void(NativeTransportState)> on_state;
    std::function<void(RouteInfo)> on_route;
    std::function<void(Bytes)> on_message;
    std::function<void(ConnectError, std::string)> on_error;
    std::function<void()> on_writable;
    std::function<void(Bytes)> on_datagram = {}; // optional
};

// Internal, unauthenticated link. Signaling and identities must be verified by
// the future connection layer before it exposes the link to an application.
// Peers must currently agree on the maximum logical message size out of band;
// a future authenticated handshake must negotiate it.
// All methods and callbacks use the same serialized FIFO executor; callback and
// lifetime requirements are inherited from NativeTransport and ReliableChannel.
class NativeReliableLink {
  public:
    NativeReliableLink(rtc::Configuration config, NativeRelayPolicy policy,
                       ReliableChannelLimits limits, MemoryBudget budget,
                       NativeReceiveLimits inbound_limits, Executor executor,
                       NativeLinkCallbacks callbacks);
    ~NativeReliableLink();
    NativeReliableLink(const NativeReliableLink &) = delete;
    NativeReliableLink &operator=(const NativeReliableLink &) = delete;
    void start_offer();
    void set_remote_description(rtc::Description description);
    void add_remote_candidate(rtc::Candidate candidate);
    [[nodiscard]] ChannelSend send(ByteView message);
    void enable_datagrams();
    [[nodiscard]] bool datagrams_available() const;
    [[nodiscard]] ChannelSend send_datagram(ByteView message);
    // See ReliableChannel::set_receive_limit; kept across channel reconfiguration.
    void set_receive_limit(std::size_t bytes);
    [[nodiscard]] std::size_t writable_threshold() const;
    [[nodiscard]] NativeTransportState state() const;
    [[nodiscard]] RouteInfo poll_route();
    void wake();
    void close();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace p2pdc::detail
