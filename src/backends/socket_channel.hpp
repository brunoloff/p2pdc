#pragma once

#include "backend_loop.hpp"
#include "backend_records.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rtc {
class WebSocket;
}

namespace p2pdc::detail {

using SocketMessage = std::variant<std::string, Bytes>;

// One WebSocket to a server, opened when first needed and opened again after it closes, no
// sooner than a backoff that doubles after each failed attempt (websocket_channel.mjs). Owners
// learn of messages and closes through handlers; whatever was in flight when the socket closed
// has failed. Methods and handlers run on the loop thread. TLS certificates are checked
// (libdatachannel's VerifiedTlsTransport) except where docs/DISCOVERY_BACKENDS.md says not.
class SocketChannel : public std::enable_shared_from_this<SocketChannel> {
  public:
    struct Handlers {
        std::function<void(SocketMessage)> message;
        std::function<void()> closed; // the open socket closed
    };
    using Ready = std::function<void(std::optional<BackendFailure>)>;

    static std::shared_ptr<SocketChannel> create(std::shared_ptr<BackendLoop> loop, std::string url,
                                                 std::vector<std::string> protocols,
                                                 Handlers handlers);
    ~SocketChannel();
    SocketChannel(const SocketChannel &) = delete;
    SocketChannel &operator=(const SocketChannel &) = delete;

    // Calls back once the socket is open, or with Transient when it cannot be.
    void ready(Ready done);
    [[nodiscard]] bool open() const;
    // False when the socket is not open.
    bool send(std::string text);
    bool send(ByteView bytes);
    // Drops the socket after a protocol error, without calling the closed handler; the next
    // ready() opens another after the backoff.
    void drop();
    // For good; the closed handler is not called, and waiting ready() calls fail.
    void close();

  private:
    SocketChannel(std::shared_ptr<BackendLoop> loop, std::string url,
                  std::vector<std::string> protocols, Handlers handlers);
    void connect();
    void opened(std::uint64_t attempt);
    void failed(std::uint64_t attempt);
    void finish(std::optional<BackendFailure> result);
    void release_socket();

    std::shared_ptr<BackendLoop> loop_;
    std::string url_;
    std::vector<std::string> protocols_;
    Handlers handlers_;
    std::shared_ptr<rtc::WebSocket> socket_;
    bool is_open_ = false;
    bool closed_ = false;
    std::uint64_t attempt_ = 0;
    BackendLoop::TimerId timer_ = 0;
    std::vector<Ready> waiting_;
    std::chrono::milliseconds retry_;
    std::chrono::steady_clock::time_point not_before_{};
};

} // namespace p2pdc::detail
