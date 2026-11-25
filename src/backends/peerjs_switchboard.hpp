#pragma once

#include "peerjs_endpoint.hpp"
#include "peerjs_host.hpp"
#include "pending_replies.hpp"
#include "record_client.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>

namespace p2pdc::detail {

// A PeerJS server as a record backend, a port of peerjs_switchboard.mjs
// (docs/DISCOVERY_BACKENDS.md). The server only relays WebRTC signalling, so each key has a
// host: whoever registers the ID derived from the key keeps its values and serves the others
// over WebRTC data connections. When the host leaves, the next store to need the key takes
// over and every store puts its own values again. The record protocol on the data channel is
// JSON objects with "p2pdc": 1 and an "op": get, put, watch or unwatch from clients, and
// values, ack, full or value from hosts.
class PeerJsSwitchboard final : public RecordClient,
                                public std::enable_shared_from_this<PeerJsSwitchboard> {
  public:
    static std::shared_ptr<PeerJsSwitchboard>
    create(std::shared_ptr<BackendLoop> loop, std::string url, std::string key,
           rtc::Configuration rtc = peerjs_rtc_configuration());
    void put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) override;
    void get(const RecordKey &key, StoreLimits limits, bool by_content, Found found) override;
    WatchId listen(const RecordKey &key, StoreLimits limits, Listener listener) override;
    void unlisten(WatchId watch) override;
    void close() override;

  private:
    using Link = PeerJsEndpoint::Link;
    // A reply from the key's host, a local answer in the same shape, or why there is none.
    using Result = std::variant<nlohmann::json, PeerJsError>;
    using Then = std::function<void(Result)>;
    struct HostEntry {
        std::string hex;
        std::shared_ptr<PeerJsEndpoint> endpoint;
        KeyHost host;
        std::map<std::string, Link> links; // by peer ID
    };
    struct HostLink {
        Link link;
        std::vector<std::function<void(std::variant<Link, PeerJsError>)>> waiting;
        std::chrono::steady_clock::time_point used;
    };
    struct OwnValue {
        std::string data;
        std::chrono::steady_clock::time_point expires;
    };
    struct Watch;

    PeerJsSwitchboard(std::shared_ptr<BackendLoop> loop, std::string url, std::string key,
                      rtc::Configuration rtc);
    std::shared_ptr<PeerJsEndpoint> endpoint(std::string id, std::function<void(Link)> incoming,
                                             std::function<void(BackendFailure)> closed);
    // Hosting (peerjs_switchboard.cpp).
    void route(const std::string &hex, std::function<void(std::variant<bool, PeerJsError>)> done);
    void claim(const std::string &hex);
    void claimed(const std::shared_ptr<HostEntry> &entry,
                 std::variant<PeerJsSocket::Registration, BackendFailure> state);
    void unhost(const std::string &hex, const std::shared_ptr<HostEntry> &entry);
    void admit(const std::string &hex, const std::shared_ptr<HostEntry> &entry, Link link);
    void serve(const std::shared_ptr<HostEntry> &entry, const Link &link,
               const nlohmann::json &payload);
    bool keep(const std::string &hex, const std::string &id, const std::string &data);
    // Reaching remote hosts (peerjs_switchboard_link.cpp).
    void with_host(const std::string &hex, std::function<nlohmann::json(HostEntry &)> local,
                   std::function<void(Then)> remote, Then done, int attempt = 0);
    void host_link(const std::string &hex,
                   std::function<void(std::variant<Link, PeerJsError>)> done);
    void attach(const std::string &hex, const std::shared_ptr<HostLink> &entry,
                std::variant<Link, PeerJsError> result);
    void lost_host(const std::string &hex, const std::shared_ptr<HostLink> &entry);
    void reput(const std::string &hex);
    void ask(const std::string &hex, nlohmann::json message, Then done);
    void reply(const std::string &hex, const nlohmann::json &payload);
    // Watches and upkeep (peerjs_switchboard_ops.cpp).
    void subscribe(const std::shared_ptr<Watch> &watch,
                   std::function<void(std::optional<PeerJsError>)> done);
    // By value: the watch may be the entry it erases.
    void end_watch(std::shared_ptr<Watch> watch, std::optional<BackendFailure> failure, bool tell);
    void sweep();

    std::shared_ptr<BackendLoop> loop_;
    std::string url_, key_;
    rtc::Configuration rtc_;
    bool closed_ = false;
    std::shared_ptr<PeerJsEndpoint> client_;
    std::map<std::string, std::shared_ptr<HostEntry>> hosts_;
    std::map<std::string, std::vector<std::function<void(std::variant<bool, PeerJsError>)>>>
        routing_;
    std::set<std::string> remote_; // keys hosted elsewhere, as far as we know
    std::map<std::string, std::shared_ptr<HostLink>> links_;
    std::map<std::string, std::map<std::string, OwnValue>> own_; // put again on host changes
    std::map<std::string, std::shared_ptr<Watch>> watches_;      // by sub
    std::map<WatchId, std::string> watch_ids_;
    std::map<std::string, std::string> waiting_; // rid -> key hex
    PendingReplies<Result> replies_;
    BackendLoop::TimerId sweeper_ = 0;
    WatchId next_watch_ = 0;
};

} // namespace p2pdc::detail
