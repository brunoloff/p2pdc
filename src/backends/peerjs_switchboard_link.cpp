// Reaching a key's host: here, or over a data connection to the store that hosts it, moving
// on to a new host when that one leaves.
#include "peerjs_switchboard_detail.hpp"

#include <iostream>

namespace p2pdc::detail {

// Runs an operation against the key's host, here or remote, once more when a remote host left.
void PeerJsSwitchboard::with_host(const std::string &hex,
                                  std::function<nlohmann::json(HostEntry &)> local,
                                  std::function<void(Then)> remote, Then done, int attempt) {
    route(hex, [self = shared_from_this(), hex, local, remote, done,
                attempt](std::variant<bool, PeerJsError> where) {
        if (const auto *error = std::get_if<PeerJsError>(&where)) {
            done(*error);
        } else if (std::get<bool>(where)) {
            done(local(*self->hosts_.at(hex)));
        } else {
            remote([self, hex, local, remote, done, attempt](Result result) {
                const auto *error = std::get_if<PeerJsError>(&result);
                if (!error || !error->gone || attempt > 0) {
                    done(std::move(result));
                    return;
                }
                self->remote_.erase(hex);
                self->with_host(hex, local, remote, done, attempt + 1);
            });
        }
    });
}

// Our data connection to the key's remote host, opened when first needed.
void PeerJsSwitchboard::host_link(const std::string &hex,
                                  std::function<void(std::variant<Link, PeerJsError>)> done) {
    if (const auto found = links_.find(hex); found != links_.end()) {
        found->second->used = std::chrono::steady_clock::now();
        if (found->second->link)
            done(found->second->link);
        else
            found->second->waiting.push_back(std::move(done));
        return;
    }
    auto entry = std::make_shared<HostLink>();
    entry->used = std::chrono::steady_clock::now();
    entry->waiting.push_back(std::move(done));
    links_[hex] = entry;
    client_->connect(peerjs::host_id(hex), [self = shared_from_this(), hex, entry](auto result) {
        self->attach(hex, entry, std::move(result));
    });
}

void PeerJsSwitchboard::attach(const std::string &hex, const std::shared_ptr<HostLink> &entry,
                               std::variant<Link, PeerJsError> result) {
    const auto found = links_.find(hex);
    const bool current = found != links_.end() && found->second == entry;
    if (auto *link = std::get_if<Link>(&result); link && !current) {
        (*link)->close();
        result = PeerJsError{BackendFailure::Transient, true};
    } else if (link) {
        entry->link = *link;
        const std::weak_ptr<PeerJsSwitchboard> weak = weak_from_this();
        (*link)->on_message = [weak, hex](const nlohmann::json &message) {
            if (auto self = weak.lock())
                self->reply(hex, message);
        };
        (*link)->on_close = [weak, hex, held = std::weak_ptr(entry)] {
            auto self = weak.lock();
            if (auto owner = held.lock(); self && owner)
                self->lost_host(hex, owner);
        };
    } else if (current) {
        links_.erase(found);
    }
    for (auto &done : std::exchange(entry->waiting, {}))
        done(result);
}

// The remote host went away: its requests fail, so that they move to a new host, and its
// watches subscribe again there.
void PeerJsSwitchboard::lost_host(const std::string &hex, const std::shared_ptr<HostLink> &entry) {
    const auto found = links_.find(hex);
    if (found == links_.end() || found->second != entry)
        return;
    links_.erase(found);
    for (const auto &[rid, waiting_hex] : decltype(waiting_)(waiting_))
        if (waiting_hex == hex)
            replies_.settle(rid, PeerJsError{BackendFailure::Transient, true});
    if (!remote_.erase(hex) || closed_)
        return;
    reput(hex);
    for (const auto &[sub, watch] : decltype(watches_)(watches_)) {
        if (watch->hex != hex)
            continue;
        subscribe(watch, [self = shared_from_this(), watch](std::optional<PeerJsError> error) {
            if (error)
                self->end_watch(watch, error->failure, true);
        });
    }
}

// Our values on a key whose host left go to its new host, which may be this store.
void PeerJsSwitchboard::reput(const std::string &hex) {
    const auto found = own_.find(hex);
    if (found == own_.end() || found->second.empty())
        return;
    auto values = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
    for (const auto &[id, value] : found->second)
        values->emplace_back(id, value.data);
    auto self = shared_from_this();
    auto send_next = std::make_shared<std::function<void(Then)>>();
    // Puts the values one after another; the first failure ends the round.
    *send_next = [self, hex, values, send_next](Then done) {
        if (values->empty()) {
            done(nlohmann::json{{"op", "ack"}});
            return;
        }
        auto [id, data] = values->back();
        values->pop_back();
        self->ask(hex, {{"op", "put"}, {"id", id}, {"data", data}},
                  [send_next, done](Result result) {
                      if (std::holds_alternative<PeerJsError>(result))
                          done(std::move(result));
                      else
                          (*send_next)(done);
                  });
    };
    with_host(
        hex, [](HostEntry &) { return nlohmann::json{{"op", "ack"}}; }, *send_next,
        [send_next](Result result) {
            if (std::holds_alternative<PeerJsError>(result))
                std::cerr << "p2pdc: could not put values again on the key's new PeerJS host\n";
            *send_next = nullptr; // ends the cycle the function holds on itself
        });
}

void PeerJsSwitchboard::ask(const std::string &hex, nlohmann::json message, Then done) {
    host_link(hex, [self = shared_from_this(), hex, message = std::move(message),
                    done = std::move(done)](std::variant<Link, PeerJsError> link) mutable {
        if (const auto *error = std::get_if<PeerJsError>(&link)) {
            done(*error);
            return;
        }
        const auto rid = random_id();
        self->waiting_[rid] = hex;
        self->replies_.wait(rid, [self, rid, done](std::variant<Result, BackendFailure> reply) {
            self->waiting_.erase(rid);
            if (const auto *failure = std::get_if<BackendFailure>(&reply))
                done(PeerJsError{*failure});
            else
                done(std::get<Result>(std::move(reply)));
        });
        message["p2pdc"] = 1;
        message["rid"] = rid;
        if (!std::get<Link>(link)->send(message))
            self->replies_.fail(rid, BackendFailure::Transient);
    });
}

// Replies and pushed values from the remote host of hex.
void PeerJsSwitchboard::reply(const std::string &hex, const nlohmann::json &payload) {
    if (!peerjs::ours(payload))
        return;
    if (peerjs::text(payload, "op") == "value") {
        const auto found = watches_.find(peerjs::text(payload, "sub"));
        if (found != watches_.end() && found->second->hex == hex)
            found->second->deliver(peerjs::text(payload, "id"), peerjs::text(payload, "data"));
        return;
    }
    const auto rid = peerjs::text(payload, "rid");
    const auto waiting = waiting_.find(rid);
    if (waiting != waiting_.end() && waiting->second == hex)
        replies_.settle(rid, payload);
}

} // namespace p2pdc::detail
