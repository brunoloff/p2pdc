#include "network_backends.hpp"

#include "../discovery/discovery_backends_json.hpp"
#include "backend_loop.hpp"
#include "client_record_store.hpp"
#include "fan_out_store.hpp"
#include "mqtt_broker.hpp"
#include "nostr_relay.hpp"
#include "peerjs_switchboard.hpp"

#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace p2pdc::detail {
namespace {
using ::p2pdc::DiscoveryBackend;

// The client for one entry, or null for the DHT, which native nodes join themselves.
std::shared_ptr<RecordClient> client_for(const DiscoveryBackend &entry,
                                         const std::shared_ptr<BackendLoop> &loop) {
    struct Maker {
        const std::shared_ptr<BackendLoop> &loop;
        std::shared_ptr<RecordClient> operator()(const ::p2pdc::OpenDhtProxies &) const {
            return nullptr;
        }
        std::shared_ptr<RecordClient> operator()(const ::p2pdc::NostrRelay &value) const {
            return NostrRelay::create(loop, value.url);
        }
        std::shared_ptr<RecordClient> operator()(const ::p2pdc::MqttBroker &value) const {
            return MqttBroker::create(loop, value.url);
        }
        std::shared_ptr<RecordClient> operator()(const ::p2pdc::PeerJsServer &value) const {
            return PeerJsSwitchboard::create(loop, value.url, value.key);
        }
    };
    return std::visit(Maker{loop}, entry);
}
} // namespace

std::shared_ptr<NetworkBackends>
NetworkBackends::create(const std::vector<DiscoveryBackend> &list) {
    validate_discovery_backends(list);
    auto loop = std::make_shared<BackendLoop>();
    std::vector<std::shared_ptr<RecordClient>> clients;
    std::set<std::string> seen; // one client per server, as entries are one line each
    for (const auto &entry : list) {
        if (!seen.insert(to_string(entry)).second)
            continue;
        if (auto client = client_for(entry, loop))
            clients.push_back(std::move(client));
    }
    if (clients.empty())
        return nullptr;
    return std::shared_ptr<NetworkBackends>(
        new NetworkBackends(std::move(loop), std::move(clients)));
}

NetworkBackends::NetworkBackends(std::shared_ptr<BackendLoop> loop,
                                 std::vector<std::shared_ptr<RecordClient>> clients)
    : loop_(std::move(loop)), clients_(std::move(clients)) {}

NetworkBackends::~NetworkBackends() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: closing discovery backends failed: " << error.what() << '\n';
    }
}

RecordStore NetworkBackends::store(RecordStore dht, StoreLimits limits) const {
    if (clients_.empty()) // closed
        return dht;
    std::vector<RecordStore> stores{std::move(dht)};
    for (const auto &client : clients_)
        stores.push_back(make_client_record_store(loop_, client, limits));
    return make_fan_out_store(loop_, std::move(stores), limits);
}

void NetworkBackends::close() {
    if (clients_.empty())
        return;
    if (loop_->on_loop_thread())
        throw std::logic_error("network backends closed from a backend callback");
    auto closed = std::make_shared<std::promise<void>>();
    auto done = closed->get_future();
    const bool posted = loop_->post([clients = clients_, closed] {
        for (const auto &client : clients)
            client->close();
        closed->set_value();
    });
    // Closing only fails what is in flight; it does not block.
    if (posted && done.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        std::cerr << "p2pdc: discovery backends did not close in time\n";
    loop_->stop();
    clients_.clear();
}

} // namespace p2pdc::detail
