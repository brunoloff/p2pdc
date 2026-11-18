#pragma once

#include "p2pdc/frame.h"
#include "p2pdc/ice_config.h"
#include "p2pdc/route.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace p2pdc::detail {
[[nodiscard]] bool browser_webrtc_available();
struct BrowserTransportImpl;
enum class BrowserRole { Offerer, Answerer };
enum class BrowserTransportState { Connecting, Ready, Suspended, Failed, Closed };
enum class BrowserTransportError { Transport, Resource };
enum class BrowserSend { Accepted, NotReady, TooLarge, Backpressure, Failed, Closed };
struct BrowserTransportCallbacks {
    std::function<void(std::string)> on_description;
    std::function<void(std::string, std::string)> on_candidate;
    std::function<void(BrowserTransportState)> on_state;
    std::function<void(Bytes)> on_binary;
    std::function<void(RouteInfo)> on_route;
    std::function<void(BrowserTransportError)> on_error;
    std::function<void()> on_writable;
    std::function<void(bool)> on_description_applied;
    std::function<void(std::uint32_t, bool)> on_candidate_applied;
    std::function<void(Bytes)> on_datagram = {}; // optional
};
struct BrowserTransportLimits {
    std::size_t message_bytes = 262145;
    std::size_t buffer_bytes = 2097152;
    std::size_t inbound_bytes = 1048576;
    std::size_t inbound_messages = 128;
};
// Raw WebRTC only; signaling and peer authentication belong to the caller.
// All methods and callbacks run on the browser event loop. Inputs are copied,
// callbacks are asynchronous, and close suppresses future callbacks. Callbacks
// must not throw. poll_route is required to refresh the selected ICE pair.
class BrowserTransport {
  public:
    BrowserTransport(BrowserRole role, IceConfig ice, BrowserTransportLimits limits,
                     BrowserTransportCallbacks callbacks);
    ~BrowserTransport();
    BrowserTransport(const BrowserTransport &) = delete;
    BrowserTransport &operator=(const BrowserTransport &) = delete;
    [[nodiscard]] bool start_offer();
    [[nodiscard]] bool set_remote_description(std::string_view sdp);
    [[nodiscard]] bool add_remote_candidate(std::uint32_t operation, std::string_view candidate,
                                            std::string_view mid);
    [[nodiscard]] BrowserSend send(ByteView bytes);
    // Call once both sides are known to support datagrams (see datagram_support.hpp).
    void enable_datagrams();
    [[nodiscard]] bool datagrams_available() const;
    [[nodiscard]] BrowserSend send_datagram(ByteView bytes);
    [[nodiscard]] std::optional<std::size_t> buffered_amount() const;
    // Zero means no backend-advertised limit; nullopt means unavailable.
    [[nodiscard]] std::optional<std::size_t> max_message_size() const;
    void poll_route();
    void close();

  private:
    std::shared_ptr<BrowserTransportImpl> state_;
};
} // namespace p2pdc::detail
