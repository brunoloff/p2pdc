#pragma once

#include "peerjs_link.hpp"
#include "peerjs_socket.hpp"

#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>

namespace p2pdc::detail {

// The PeerJS client's own ICE servers (peers/peerjs lib/util.ts): Google's STUN server and
// the public TURN relays PeerJS runs for its users.
[[nodiscard]] rtc::Configuration peerjs_rtc_configuration();

// An ID on a PeerJS server that opens data connections to other IDs and, given an incoming
// handler, accepts them, a port of peerjs_endpoint.mjs. Signalling has the shapes the PeerJS
// client sends: the offerer creates the channel, OFFER and ANSWER carry the descriptions,
// CANDIDATE each ICE candidate; candidates that arrive before the remote description wait for
// it. Open data connections no longer depend on the server. Loop thread only.
class PeerJsEndpoint : public std::enable_shared_from_this<PeerJsEndpoint> {
  public:
    using Link = std::shared_ptr<PeerJsLink>;
    using Connected = std::function<void(std::variant<Link, PeerJsError>)>;
    struct Options {
        rtc::Configuration rtc;
        std::size_t max_links;
        std::function<void(Link)> incoming;         // null: refuse offers
        std::function<void(BackendFailure)> closed; // the server connection closed
    };
    static std::shared_ptr<PeerJsEndpoint> create(std::shared_ptr<BackendLoop> loop,
                                                  const std::string &url, const std::string &key,
                                                  std::string id, Options options);
    ~PeerJsEndpoint();
    PeerJsEndpoint(const PeerJsEndpoint &) = delete;
    PeerJsEndpoint &operator=(const PeerJsEndpoint &) = delete;

    void register_id(PeerJsSocket::Registered done) { socket_->register_id(std::move(done)); }
    // A link to peer, or gone when the server says peer is not there.
    void connect(const std::string &peer, Connected done);
    void close();

  private:
    struct Negotiation;
    PeerJsEndpoint(std::shared_ptr<BackendLoop> loop, Options options);
    std::shared_ptr<Negotiation> negotiate(const std::string &peer, const std::string &id,
                                           Connected done);
    void offer(const std::shared_ptr<Negotiation> &negotiation);
    void answer(const std::string &peer, const nlohmann::json &payload);
    void signal(const nlohmann::json &message);
    void described(const std::string &id, const std::string &sdp, const std::string &type);
    void candidate(const std::string &id, const nlohmann::json &value);
    void local_candidate(const std::string &id, const rtc::Candidate &value);
    void local_description(const std::string &id, const std::string &sdp, const std::string &type);
    void opened(const std::string &id, std::shared_ptr<rtc::DataChannel> channel);
    void fail(const std::string &id, PeerJsError error);
    void gone(const std::string &peer);
    void lost(BackendFailure failure);

    std::shared_ptr<BackendLoop> loop_;
    Options options_;
    std::shared_ptr<PeerJsSocket> socket_;
    std::map<std::string, std::shared_ptr<Negotiation>> negotiations_;
    std::set<Link> links_;
};

} // namespace p2pdc::detail
