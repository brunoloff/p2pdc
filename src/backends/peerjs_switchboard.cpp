// Hosting keys: claiming a key's PeerJS ID, serving other stores over data connections, and
// giving the key up. The other parts of PeerJsSwitchboard are in peerjs_switchboard_link.cpp
// and peerjs_switchboard_ops.cpp.
#include "peerjs_switchboard.hpp"
#include "peerjs_switchboard_detail.hpp"

namespace p2pdc::detail {

std::shared_ptr<PeerJsSwitchboard> PeerJsSwitchboard::create(std::shared_ptr<BackendLoop> loop,
                                                             std::string url, std::string key,
                                                             rtc::Configuration rtc) {
    auto board = std::shared_ptr<PeerJsSwitchboard>(
        new PeerJsSwitchboard(std::move(loop), std::move(url), std::move(key), std::move(rtc)));
    board->client_ = board->endpoint("p2pdcc" + random_id(), nullptr, nullptr);
    board->sweep();
    return board;
}

PeerJsSwitchboard::PeerJsSwitchboard(std::shared_ptr<BackendLoop> loop, std::string url,
                                     std::string key, rtc::Configuration rtc)
    : loop_(loop), url_(std::move(url)), key_(std::move(key)), rtc_(std::move(rtc)),
      replies_(std::move(loop)) {}

std::shared_ptr<PeerJsEndpoint>
PeerJsSwitchboard::endpoint(std::string id, std::function<void(Link)> incoming,
                            std::function<void(BackendFailure)> closed) {
    return PeerJsEndpoint::create(
        loop_, url_, key_, std::move(id),
        {rtc_, peerjs::max_host_links, std::move(incoming), std::move(closed)});
}

// Hosts the key when its ID is free; otherwise it is hosted elsewhere. true: hosted here.
void PeerJsSwitchboard::route(const std::string &hex,
                              std::function<void(std::variant<bool, PeerJsError>)> done) {
    if (closed_) {
        done(PeerJsError{BackendFailure::Transient});
    } else if (hosts_.contains(hex) || remote_.contains(hex)) {
        done(hosts_.contains(hex));
    } else {
        auto &waiting = routing_[hex];
        waiting.push_back(std::move(done));
        if (waiting.size() == 1)
            claim(hex);
    }
}

void PeerJsSwitchboard::claim(const std::string &hex) {
    auto entry = std::make_shared<HostEntry>();
    entry->hex = hex;
    const std::weak_ptr<PeerJsSwitchboard> weak = weak_from_this();
    const std::weak_ptr<HostEntry> held = entry;
    entry->endpoint = endpoint(
        peerjs::host_id(hex),
        [weak, held, hex](Link link) {
            auto self = weak.lock();
            if (auto owner = held.lock(); self && owner)
                self->admit(hex, owner, std::move(link));
            else
                link->close();
        },
        [weak, held, hex](BackendFailure) {
            auto self = weak.lock();
            if (auto owner = held.lock(); self && owner)
                self->unhost(hex, owner);
        });
    entry->endpoint->register_id(
        [self = shared_from_this(), entry](auto state) { self->claimed(entry, state); });
}

void PeerJsSwitchboard::claimed(const std::shared_ptr<HostEntry> &entry,
                                std::variant<PeerJsSocket::Registration, BackendFailure> state) {
    const auto &hex = entry->hex;
    std::variant<bool, PeerJsError> route;
    if (const auto *failure = std::get_if<BackendFailure>(&state);
        failure && *failure != BackendFailure::Transient) {
        entry->endpoint->close();
        route = PeerJsError{*failure};
    } else if (failure) {
        // A server may close a second connection for a taken ID before ID-TAKEN arrives, as
        // 0.peerjs.com does at times: ask the host. If there is none, the server expires the
        // offer and the key is claimed again.
        entry->endpoint->close();
        remote_.insert(hex);
        route = false;
    } else if (std::get<PeerJsSocket::Registration>(state) == PeerJsSocket::Registration::Taken ||
               closed_) {
        entry->endpoint->close();
        remote_.insert(hex);
        route = false;
    } else {
        hosts_[hex] = entry;
        const auto now = std::chrono::steady_clock::now();
        for (const auto &[id, value] : own_[hex])
            entry->host.put(
                id, value.data,
                std::chrono::duration_cast<std::chrono::milliseconds>(value.expires - now));
        route = true;
    }
    for (auto &done : std::exchange(routing_[hex], {}))
        done(route);
    routing_.erase(hex);
}

// Losing the hosting ID ends the watches on it, which are watched again.
void PeerJsSwitchboard::unhost(const std::string &hex, const std::shared_ptr<HostEntry> &entry) {
    const auto found = hosts_.find(hex);
    if (found == hosts_.end() || found->second != entry)
        return;
    hosts_.erase(found);
    entry->endpoint->close();
    for (const auto &[sub, watch] : decltype(watches_)(watches_))
        if (watch->hex == hex)
            end_watch(watch, BackendFailure::Transient, true);
}

// A data connection to a key this store hosts; one per peer.
void PeerJsSwitchboard::admit(const std::string &hex, const std::shared_ptr<HostEntry> &entry,
                              Link link) {
    const auto found = hosts_.find(hex);
    if (found == hosts_.end() || found->second != entry) {
        link->close();
        return;
    }
    if (const auto previous = entry->links.find(link->peer()); previous != entry->links.end())
        previous->second->close();
    entry->links[link->peer()] = link;
    const std::weak_ptr<PeerJsSwitchboard> weak = weak_from_this();
    const std::weak_ptr<HostEntry> held = entry;
    const std::weak_ptr<PeerJsLink> self_link = link;
    link->on_message = [weak, held, self_link](const nlohmann::json &message) {
        auto self = weak.lock();
        auto owner = held.lock();
        if (auto open = self_link.lock(); self && owner && open)
            self->serve(owner, open, message);
    };
    link->on_close = [held, self_link, peer = link->peer()] {
        auto owner = held.lock();
        if (!owner)
            return;
        const auto found = owner->links.find(peer);
        if (found == owner->links.end() || found->second != self_link.lock())
            return;
        owner->links.erase(found);
        owner->host.forget(peer);
    };
}

// A request over a data connection to a key this store hosts.
void PeerJsSwitchboard::serve(const std::shared_ptr<HostEntry> &entry, const Link &link,
                              const nlohmann::json &payload) {
    const auto found = hosts_.find(entry->hex);
    if (!peerjs::ours(payload) || found == hosts_.end() || found->second != entry)
        return;
    const auto op = peerjs::text(payload, "op");
    if (op == "unwatch") {
        entry->host.unwatch(link->peer(), peerjs::text(payload, "sub"));
        return;
    }
    if (!payload.contains("rid") || !payload["rid"].is_string())
        return;
    const auto answer = [&](nlohmann::json message) {
        message["p2pdc"] = 1;
        message["rid"] = payload["rid"];
        link->send(message);
    };
    if (op == "get") {
        answer({{"op", "values"}, {"values", peerjs::values_json(entry->host.values())}});
    } else if (op == "watch") {
        answer({{"op",
                 entry->host.watch(link->peer(), peerjs::text(payload, "sub")) ? "ack" : "full"}});
    } else if (op == "put" && payload.contains("id") && payload["id"].is_string() &&
               payload.contains("data") && payload["data"].is_string() &&
               payload["data"].get_ref<const std::string &>().size() <= peerjs::max_data_chars) {
        const bool kept =
            keep(entry->hex, payload["id"].get<std::string>(), payload["data"].get<std::string>());
        answer({{"op", kept ? "ack" : "full"}});
    }
}

// Keeps a value on a key this store hosts and tells its watchers; false when the key is full.
bool PeerJsSwitchboard::keep(const std::string &hex, const std::string &id,
                             const std::string &data) {
    const auto found = hosts_.find(hex);
    if (found == hosts_.end())
        return false;
    auto entry = found->second;
    const auto watchers = entry->host.put(id, data, record_lifetime);
    if (!watchers)
        return false;
    for (const auto &watcher : *watchers) {
        const auto link = entry->links.find(watcher.client);
        if (link != entry->links.end())
            link->second->send(
                {{"p2pdc", 1}, {"op", "value"}, {"sub", watcher.sub}, {"id", id}, {"data", data}});
    }
    return true;
}

} // namespace p2pdc::detail
