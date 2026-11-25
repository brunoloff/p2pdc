#pragma once

#include "socket_channel.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace p2pdc::detail {

// Why a PeerJS request failed. gone: the other side is not on the server, or its data
// connection closed, so the request may go to a new host (peerjs_endpoint.mjs, PeerGone).
struct PeerJsError {
    BackendFailure failure = BackendFailure::Transient;
    bool gone = false;
};

// One ID registered with a PeerJS server (peers/peerjs-server), a port of peerjs_socket.mjs.
// The server relays WebRTC signalling (OFFER, ANSWER, CANDIDATE) between IDs, setting each
// message's sender; it keeps messages for an absent ID a few seconds, then tells the sender
// with EXPIRE. Loop thread only.
class PeerJsSocket : public std::enable_shared_from_this<PeerJsSocket> {
  public:
    enum class Registration { Open, Taken };
    using Registered = std::function<void(std::variant<Registration, BackendFailure>)>;
    struct Handlers {
        std::function<void(const nlohmann::json &)> signal; // OFFER, ANSWER or CANDIDATE
        std::function<void(const std::string &)> gone;      // EXPIRE or LEAVE from that ID
        std::function<void(BackendFailure)> closed;         // the registered socket closed
    };
    static std::shared_ptr<PeerJsSocket> create(std::shared_ptr<BackendLoop> loop,
                                                const std::string &url, const std::string &key,
                                                std::string id, Handlers handlers);
    ~PeerJsSocket();
    PeerJsSocket(const PeerJsSocket &) = delete;
    PeerJsSocket &operator=(const PeerJsSocket &) = delete;

    // Open once the ID is ours, Taken when another client holds it. A failure, or a lost
    // connection, lets the next call try again, under the same ID and token.
    void register_id(Registered done);
    // type is OFFER, ANSWER or CANDIDATE, with a payload as the PeerJS client sends it.
    bool send(const std::string &type, const std::string &dst, nlohmann::json payload);
    [[nodiscard]] const std::string &id() const { return id_; }
    void close();

  private:
    PeerJsSocket(std::shared_ptr<BackendLoop> loop, std::string id, Handlers handlers);
    void receive(SocketMessage data);
    void received(const nlohmann::json &message);
    void finish(std::variant<Registration, BackendFailure> result);
    void lost();
    void beat();

    std::shared_ptr<BackendLoop> loop_;
    std::string id_;
    Handlers handlers_;
    std::shared_ptr<SocketChannel> channel_;
    std::optional<Registration> registration_;
    std::vector<Registered> waiting_;
    BackendLoop::TimerId timer_ = 0, heartbeat_ = 0;
};

// The WebSocket URL of a PeerJS server for one ID: url with its query replaced by key, id and
// token, as the PeerJS client builds it.
[[nodiscard]] std::string peerjs_socket_url(const std::string &url, const std::string &key,
                                            const std::string &id, const std::string &token);

} // namespace p2pdc::detail
