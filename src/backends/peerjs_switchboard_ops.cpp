// The record backend operations of PeerJsSwitchboard, its watches and its upkeep.
#include "peerjs_switchboard_detail.hpp"

namespace p2pdc::detail {
namespace {
std::optional<BackendFailure>
acknowledged(const std::variant<nlohmann::json, PeerJsError> &result) {
    if (const auto *error = std::get_if<PeerJsError>(&result))
        return error->failure;
    // "full": the key's host is at its cap, which may pass.
    return peerjs::text(std::get<nlohmann::json>(result), "op") == "ack"
               ? std::nullopt
               : std::optional(BackendFailure::Transient);
}
} // namespace

void PeerJsSwitchboard::put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) {
    if (value.size() > limits.max_value_bytes)
        throw std::invalid_argument("oversized PeerJS payload");
    const auto hex = to_hex(key), id = random_id(), data = encode_base64(value);
    own_[hex][id] = OwnValue{data, std::chrono::steady_clock::now() + record_lifetime};
    auto self = shared_from_this();
    with_host(
        hex,
        [self, hex, id, data](HostEntry &) {
            return nlohmann::json{{"op", self->keep(hex, id, data) ? "ack" : "full"}};
        },
        [self, hex, id, data](Then then) {
            self->ask(hex, {{"op", "put"}, {"id", id}, {"data", data}}, std::move(then));
        },
        [done = std::move(done)](Result result) { done(acknowledged(result)); });
}

void PeerJsSwitchboard::get(const RecordKey &key, StoreLimits limits, bool by_content,
                            Found found) {
    const auto hex = to_hex(key);
    auto self = shared_from_this();
    with_host(
        hex,
        [](HostEntry &entry) {
            return nlohmann::json{{"op", "values"},
                                  {"values", peerjs::values_json(entry.host.values())}};
        },
        [self, hex](Then then) { self->ask(hex, {{"op", "get"}}, std::move(then)); },
        [limits, by_content, found = std::move(found)](Result result) {
            if (const auto *error = std::get_if<PeerJsError>(&result)) {
                found(error->failure);
                return;
            }
            RecordCollector collector(limits, by_content);
            const auto &reply = std::get<nlohmann::json>(result);
            const auto values = reply.find("values");
            for (const auto &value :
                 values != reply.end() && values->is_array() ? *values : nlohmann::json::array()) {
                if (!value.is_object())
                    continue;
                auto bytes = decode_base64(peerjs::text(value, "data"), limits.max_value_bytes);
                if (bytes && !collector.add({std::move(*bytes), peerjs::text(value, "id")}))
                    break;
            }
            found(collector.take());
        });
}

RecordClient::WatchId PeerJsSwitchboard::listen(const RecordKey &key, StoreLimits limits,
                                                Listener listener) {
    auto watch =
        std::make_shared<Watch>(Watch{random_id(), to_hex(key), limits, std::move(listener)});
    const auto id = ++next_watch_;
    watches_[watch->sub] = watch;
    watch_ids_[id] = watch->sub;
    auto self = shared_from_this();
    // Watchers renew their lease on the host every 30 s; a failed renewal ends the watch.
    watch->renewal = [weak = weak_from_this(), held = std::weak_ptr(watch)] {
        auto owner = weak.lock();
        auto current = held.lock();
        if (!owner || !current || current->ended)
            return;
        current->renew = owner->loop_->post_after(peerjs::renew_interval, [weak, held] {
            auto board = weak.lock();
            auto watching = held.lock();
            if (!board || !watching || watching->ended)
                return;
            board->subscribe(watching, [board, held](std::optional<PeerJsError> error) {
                auto again = held.lock();
                if (!again)
                    return;
                if (error)
                    board->end_watch(again, error->failure, true);
                else
                    again->renewal();
            });
        });
    };
    subscribe(watch, [self, watch](std::optional<PeerJsError> error) {
        if (error) {
            self->end_watch(watch, error->failure, true);
            return;
        }
        if (!watch->ended && !std::exchange(watch->readied, true))
            watch->listener.ready(true);
        watch->renewal();
    });
    return id;
}

void PeerJsSwitchboard::subscribe(const std::shared_ptr<Watch> &watch,
                                  std::function<void(std::optional<PeerJsError>)> done) {
    auto self = shared_from_this();
    const std::weak_ptr<Watch> held = watch;
    with_host(
        watch->hex,
        [held](HostEntry &entry) {
            if (auto current = held.lock())
                entry.host.listeners[current->sub] = [held](const std::string &id,
                                                            const std::string &data) {
                    if (auto watching = held.lock())
                        watching->deliver(id, data);
                };
            return nlohmann::json{{"op", "ack"}};
        },
        [self, hex = watch->hex, sub = watch->sub](Then then) {
            self->ask(hex, {{"op", "watch"}, {"sub", sub}}, std::move(then));
        },
        [done = std::move(done)](Result result) {
            const auto failure = acknowledged(result);
            done(failure ? std::optional(PeerJsError{*failure}) : std::nullopt);
        });
}

void PeerJsSwitchboard::end_watch(std::shared_ptr<Watch> watch,
                                  std::optional<BackendFailure> failure, bool tell) {
    if (std::exchange(watch->ended, true))
        return;
    loop_->cancel(watch->renew);
    watches_.erase(watch->sub);
    std::erase_if(watch_ids_, [&](const auto &entry) { return entry.second == watch->sub; });
    if (const auto host = hosts_.find(watch->hex); host != hosts_.end())
        host->second->host.listeners.erase(watch->sub);
    const auto link = links_.find(watch->hex);
    if (remote_.contains(watch->hex) && link != links_.end() && link->second->link)
        link->second->link->send({{"p2pdc", 1}, {"op", "unwatch"}, {"sub", watch->sub}});
    if (!tell)
        return;
    if (!std::exchange(watch->readied, true))
        watch->listener.ready(false);
    watch->listener.ended(failure);
}

void PeerJsSwitchboard::unlisten(WatchId watch) {
    const auto id = watch_ids_.find(watch);
    if (id == watch_ids_.end())
        return;
    const auto found = watches_.find(id->second);
    if (found != watches_.end())
        end_watch(found->second, std::nullopt, false);
}

// Forgets expired values of our own, gives up keys no one uses here, and closes idle links.
void PeerJsSwitchboard::sweep() {
    const auto now = std::chrono::steady_clock::now();
    for (auto &[hex, values] : own_)
        std::erase_if(values, [&](const auto &entry) { return entry.second.expires <= now; });
    std::erase_if(own_, [](const auto &entry) { return entry.second.empty(); });
    for (const auto &[hex, entry] : decltype(hosts_)(hosts_)) {
        if (entry->host.idle() && !own_.contains(hex)) {
            hosts_.erase(hex);
            entry->endpoint->close();
        }
    }
    std::set<std::string> busy;
    for (const auto &[sub, watch] : watches_)
        busy.insert(watch->hex);
    for (const auto &[rid, hex] : waiting_)
        busy.insert(hex);
    for (const auto &[hex, entry] : decltype(links_)(links_)) {
        if (entry->link && !busy.contains(hex) && now - entry->used > peerjs::idle_host_link) {
            links_.erase(hex);
            entry->link->close();
        }
    }
    sweeper_ = loop_->post_after(peerjs::sweep_interval, [weak = weak_from_this()] {
        if (auto self = weak.lock(); self && !self->closed_)
            self->sweep();
    });
}

void PeerJsSwitchboard::close() {
    closed_ = true;
    loop_->cancel(sweeper_);
    for (const auto &[sub, watch] : decltype(watches_)(watches_))
        end_watch(watch, std::nullopt, true);
    for (const auto &[hex, entry] : std::exchange(hosts_, {}))
        entry->endpoint->close();
    for (const auto &[hex, entry] : std::exchange(links_, {}))
        if (entry->link)
            entry->link->close();
    client_->close();
    replies_.fail_all(BackendFailure::Transient);
}

} // namespace p2pdc::detail
