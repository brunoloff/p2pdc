#pragma once

#include "backend_loop.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <memory>
#include <string>

namespace rtc {
class DataChannel;
class PeerConnection;
} // namespace rtc

namespace p2pdc::detail {

// A PeerJS data connection, once its channel is open, a port of peerjs_link.mjs: JSON objects
// both ways, each sent as fragments of at most 16384 characters marked "+" while more follows
// and "=" on the last, until either side closes it or WebRTC gives up on the path. The owner
// sets the handlers; they and every method run on the loop thread.
class PeerJsLink : public std::enable_shared_from_this<PeerJsLink> {
  public:
    static std::shared_ptr<PeerJsLink> create(std::shared_ptr<BackendLoop> loop, std::string peer,
                                              std::shared_ptr<rtc::PeerConnection> connection,
                                              std::shared_ptr<rtc::DataChannel> channel);
    ~PeerJsLink();
    PeerJsLink(const PeerJsLink &) = delete;
    PeerJsLink &operator=(const PeerJsLink &) = delete;

    // False, and closes the link, when the message cannot go: closed, too long, or the channel
    // is not draining.
    bool send(const nlohmann::json &message);
    void close();
    [[nodiscard]] const std::string &peer() const { return peer_; }
    [[nodiscard]] bool closed() const { return closed_; }

    std::function<void(const nlohmann::json &)> on_message = [](const nlohmann::json &) {};
    std::function<void()> on_close = [] {};
    std::function<void()> released = [] {}; // for the endpoint that opened it

  private:
    PeerJsLink(std::shared_ptr<BackendLoop> loop, std::string peer,
               std::shared_ptr<rtc::PeerConnection> connection,
               std::shared_ptr<rtc::DataChannel> channel);
    void attach();
    void receive(std::string fragment);

    std::shared_ptr<BackendLoop> loop_;
    std::string peer_;
    std::shared_ptr<rtc::PeerConnection> connection_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::string partial_;
    bool closed_ = false;
};

} // namespace p2pdc::detail
