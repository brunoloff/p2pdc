#pragma once

#include "endpoint_identity.hpp"
#include "p2pdc/discovery.h"

#include <optional>
#include <vector>

namespace p2pdc::detail {
struct LeaseSnapshot {
    AnnouncementEndpoint endpoint;
    std::shared_ptr<const Bytes> capability;
    DiscoveryProtocol protocol = DiscoveryProtocol::FixedKeyV1;
};

struct PeerSnapshot {
    Advertisement peer;
    std::shared_ptr<const Bytes> capability;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
    DiscoveryProtocol protocol = DiscoveryProtocol::FixedKeyV1;
};

// A point-in-time snapshot; revalidate the exact peer after consent delays.
struct PeerCollection {
    std::vector<Advertisement> peers;
    std::shared_ptr<const Bytes> capability;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
    DiscoveryProtocol protocol = DiscoveryProtocol::FixedKeyV1;
};

class DiscoveryBackend {
  public:
    virtual ~DiscoveryBackend() = default;
    virtual void close() = 0;
    [[nodiscard]] virtual bool healthy() const = 0;
    [[nodiscard]] virtual bool recovering() const { return false; }
    [[nodiscard]] virtual bool is_current(const Advertisement &) { return false; }
    [[nodiscard]] virtual std::optional<PeerSnapshot> peer_snapshot(const Advertisement &) {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<PeerCollection> current_peers() { return std::nullopt; }
    [[nodiscard]] virtual std::optional<LeaseSnapshot> lease_snapshot() { return std::nullopt; }
};
} // namespace p2pdc::detail
