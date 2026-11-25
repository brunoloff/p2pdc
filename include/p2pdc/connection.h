#pragma once

#include "p2pdc/frame.h"
#include "p2pdc/ice_config.h"
#include "p2pdc/route.h"
#include "p2pdc/scheduler.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace p2pdc {
namespace detail {
class ConnectionBackend;
}

enum class ConnectError {
    NotReady,
    Unsupported,
    InvalidPeer,
    ResourceLimit,
    Backend,
    Scheduler,
    Timeout,
    Callback,
    Signaling,
    Protocol,
    Transport,
    Rejected,
    Canceled,
    Superseded
};
enum class ConnectionState { Connecting, Ready, Suspended, Failed, Closed };
enum class ConnectionStatus { SignalingUnavailable };
enum class SendStatus { Accepted, NotReady, TooLarge, Backpressure, Failed, Closed };

// Largest payload of one datagram. Datagrams stay within a single network packet, since
// losing any fragment of a larger one would lose all of it.
inline constexpr std::size_t max_datagram_bytes = 1024;

struct ConnectOptions {
    std::chrono::milliseconds timeout{30000};
    std::chrono::milliseconds poll_interval{100};
    std::size_t max_pending_events = 128;
    std::size_t max_pending_bytes = 1024 * 1024;
    // Local receive limit, negotiated with the peer. Must be 1 byte..4 MiB and
    // no greater than max_pending_bytes.
    std::uint32_t max_message_bytes = 256 * 1024;
    // Offer the datagram channel. It is used only if the peer offers it too.
    bool datagrams = true;
};

struct ConnectionHandlers {
    // All handlers except on_status are required. Connecting is the initial state and is not
    // delivered as an event. A handler exception fails delivery and propagates.
    std::function<void(ConnectionState)> on_state;
    std::function<void(RouteInfo)> on_route;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(ConnectError)> on_error;
    // The authenticated channel remains usable; further signaling is disabled
    // for this attempt, so a later ICE restart cannot exchange new candidates.
    std::function<void(ConnectionStatus)> on_status{};
    // Optional: a datagram from the authenticated peer. See Connection::send_datagram.
    std::function<void(Bytes)> on_datagram{};
};

// Owner-executor only. Callbacks are deferred; close suppresses undelivered
// callbacks. Accepted means locally queued, not delivered remotely. The route
// reports observed direct/TURN status; Unknown does not prove TURN was unused.
// Poll state(): scheduler submission failure can prevent an error callback;
// close a failed connection to release resources. The scheduler and executor
// must outlive pending work and must not synchronously reenter the node.
class Connection {
  public:
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    [[nodiscard]] ConnectionState state() const;
    [[nodiscard]] RouteInfo route() const;
    // The route's smoothed round-trip time (RouteInfo::round_trip_time); empty until measured.
    [[nodiscard]] std::optional<std::chrono::milliseconds> round_trip_time() const;
    // Authenticated, negotiated maximum application-message bytes. Remains
    // available while suspended; empty before authentication and after failure
    // or closure. A value does not guarantee immediate send capacity.
    [[nodiscard]] std::optional<std::uint32_t> max_message_size() const;
    [[nodiscard]] SendStatus send(ByteView message);
    // True while datagrams can be sent: both sides support them and the channel is open.
    // It becomes true shortly after Ready, or never with a peer lacking support.
    [[nodiscard]] bool datagrams_available() const;
    // Sends one message of at most max_datagram_bytes without delivery or ordering
    // guarantees: it may be lost or arrive out of order, and
    // is dropped when either side is busy. Accepted means only that it was handed to the
    // network. NotReady while unavailable, Backpressure when too much is waiting.
    [[nodiscard]] SendStatus send_datagram(ByteView message);
    void close();

  private:
    friend class Node;
    explicit Connection(std::shared_ptr<detail::ConnectionBackend> state);
    std::shared_ptr<detail::ConnectionBackend> state_;
};

using ConnectResult = std::variant<std::unique_ptr<Connection>, ConnectError>;
} // namespace p2pdc
