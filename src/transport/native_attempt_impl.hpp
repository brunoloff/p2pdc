#pragma once

#include "native_attempt.hpp"

#include <utility>

namespace p2pdc::detail {

struct NativeAttempt::Impl : HandshakeTransport, std::enable_shared_from_this<Impl> {
    NativeAttemptConfig config;
    Executor executor;
    NativeAttemptCallbacks callbacks;
    std::unique_ptr<NativeReliableLink> link;
    std::unique_ptr<NativeSignaling> signaling;
    std::unique_ptr<NativeSignalPipe> pipe;
    std::unique_ptr<HandshakeGate> gate;
    NativeAttemptState status = NativeAttemptState::Waiting;
    ConnectError failure_reason = ConnectError::Backend;
    std::optional<std::uint32_t> message_limit;
    bool started = false;
    bool transport_ready = false;
    bool authenticated = false;
    bool signaling_unavailable = false;
    bool cleaned = false;

    Impl(NativeAttemptConfig value, Executor dispatch, NativeAttemptCallbacks handlers)
        : config(std::move(value)), executor(std::move(dispatch)), callbacks(std::move(handlers)) {}
    [[nodiscard]] std::optional<StoreError> setup();
    [[nodiscard]] NativeLinkCallbacks link_callbacks();
    [[nodiscard]] SignalPipeCallbacks pipe_callbacks();
    [[nodiscard]] HandshakeGateCallbacks gate_callbacks();
    [[nodiscard]] SignalingPolicy policy() const;
    [[nodiscard]] std::uint64_t expiry(std::uint64_t now) const;
    void on_description(rtc::Description value);
    void on_candidate(rtc::Candidate value);
    void on_record(Bytes value);
    void signal_failure(bool transient, ConnectError reason = ConnectError::Signaling);
    void on_catchup(bool success);
    void on_transport(NativeTransportState value);
    void on_datagram(Bytes value);
    [[nodiscard]] bool apply_accepted_offer();
    void activate();
    void fail(ConnectError reason = ConnectError::Backend);
    void finish_failure();
    void cancel_remote() noexcept;
    void stop();
    [[nodiscard]] ChannelSend send(ByteView value) override { return link->send(value); }
    void limit_receive(std::size_t bytes) override { link->set_receive_limit(bytes); }
    void close() override { link->close(); }
};

} // namespace p2pdc::detail
