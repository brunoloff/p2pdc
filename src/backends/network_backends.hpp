#pragma once

#include "../discovery/record_store.hpp"
#include "p2pdc/discovery_backends.h"

#include <memory>
#include <vector>

namespace p2pdc::detail {

class BackendLoop;
class RecordClient;

// A native node's network meeting places: the Nostr relays, MQTT brokers and PeerJS servers of
// its backend list, each reached through one connection shared by every store of the node
// (docs/DISCOVERY_BACKENDS.md). OpenDHT entries are left out: native nodes join the DHT
// themselves. Connections open when first used.
class NetworkBackends {
  public:
    // Null when the list names no network backend. Throws std::invalid_argument for a list
    // a node refuses: more than max_discovery_backends entries, or an invalid one.
    static std::shared_ptr<NetworkBackends>
    create(const std::vector<::p2pdc::DiscoveryBackend> &list);
    ~NetworkBackends();
    NetworkBackends(const NetworkBackends &) = delete;
    NetworkBackends &operator=(const NetworkBackends &) = delete;

    // The DHT store and every network backend, used all at once, with these limits.
    [[nodiscard]] RecordStore store(RecordStore dht, StoreLimits limits) const;
    // Closes every connection, failing what is in flight, and stops the loop. Not from a
    // backend callback.
    void close();

  private:
    NetworkBackends(std::shared_ptr<BackendLoop> loop,
                    std::vector<std::shared_ptr<RecordClient>> clients);
    std::shared_ptr<BackendLoop> loop_;
    std::vector<std::shared_ptr<RecordClient>> clients_;
};

} // namespace p2pdc::detail
