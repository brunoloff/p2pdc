#pragma once

#include "p2pdc/connection.h"
#include "p2pdc/frame.h"
#include "p2pdc/memory_budget.h"
#include "p2pdc/operation.h"
#include "p2pdc/route.h"

#include <rtc/peerconnection.hpp>

namespace p2pdc::detail {

enum class NativeRelayPolicy { Automatic, RequireLocalRelay };
enum class NativeTransportState { Connecting, Ready, Suspended, Failed, Closed };
enum class NativeSendResult { Accepted, NotReady, TooLarge, Failed };

struct NativeTransportCallbacks {
    std::function<void(rtc::Description)> on_description;
    std::function<void(rtc::Candidate)> on_candidate;
    std::function<void(NativeTransportState)> on_state;
    std::function<void(Bytes)> on_binary;
    std::function<void(RouteInfo)> on_route;
    std::function<void(ConnectError, std::string)> on_error;
    std::function<void()> on_writable;
};

struct NativeReceiveLimits {
    MemoryBudget budget{4U * 1024U * 1024U};
    std::size_t max_bytes = 4U * 1024U * 1024U;
    std::size_t max_messages = 4096;
    std::size_t max_message_size = 1024U * 1024U;
};

// RequireLocalRelay requests libnice force-relay and gates application traffic on
// a verified selected local relay. Relay-only network behavior still needs
// independent verification; a selected-pair snapshot does not prove privacy.
// Internal raw transport: no peer identity, signaling authentication or protocol
// handshake is provided. Caller must authenticate before accepting application data.
// All methods, close/destruction and delivered callbacks belong to one serialized
// caller context. Executor submissions may come from backend threads; it must post
// asynchronously without blocking for caller execution, serialize execution in FIFO order,
// never throw, and outlive this adapter.
// User callbacks must not throw. close/destruction suppress queued notifications.
// A remote close or failure may discard messages queued but not yet delivered.
// Payload limits exclude backend buffers, allocator overhead, and executor tasks.
class NativeTransport {
  public:
    NativeTransport(rtc::Configuration config, NativeRelayPolicy policy, Executor executor,
                    NativeTransportCallbacks callbacks, NativeReceiveLimits limits = {});
    ~NativeTransport();
    NativeTransport(const NativeTransport &) = delete;
    NativeTransport &operator=(const NativeTransport &) = delete;
    NativeTransport(NativeTransport &&) = delete;
    NativeTransport &operator=(NativeTransport &&) = delete;

    // One initial negotiation only; glare/renegotiation is rejected. Invalid
    // signaling and backend signaling errors throw; caller decides how to abort.
    void start_offer();
    void set_remote_description(rtc::Description description);
    void add_remote_candidate(rtc::Candidate candidate);
    // Requires the patched backend; buffered messages count as locally accepted.
    // Remote delivery is not guaranteed by local acceptance.
    // Sends one backend message; fragmentation/backpressure budgets belong above.
    [[nodiscard]] NativeSendResult send(ByteView bytes);
    [[nodiscard]] std::size_t buffered_amount() const;
    // on_writable fires when the buffered amount falls to this many bytes. The
    // default 0 waits for a fully drained buffer (stop-and-wait).
    void set_writable_threshold(std::size_t bytes);
    [[nodiscard]] std::size_t writable_threshold() const;
    [[nodiscard]] std::size_t max_message_size() const;
    [[nodiscard]] NativeTransportState state() const;
    // Owner must poll periodically and after observation gaps. Polling never proves
    // complete history; TurnHistory::NotUsed is deliberately never asserted here.
    [[nodiscard]] RouteInfo poll_route();
    void close();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace p2pdc::detail
