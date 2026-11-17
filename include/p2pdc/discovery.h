#pragma once

#include "p2pdc/advertisement.h"
#include "p2pdc/discovery_key.h"
#include "p2pdc/scheduler.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace p2pdc {
namespace detail {
class DiscoveryBackend;
struct ConnectionAccess;
} // namespace detail

enum class PeerEvent { Seen, Updated, Expired };
enum class DiscoveryStatus { Reconnecting, Ready };
enum class WatchError {
    NotReady,
    Unsupported,
    ResourceLimit,
    Backend,
    Crypto,
    Clock,
    Scheduler,
    Callback
};

// The protocols use different discovery keys and do not interoperate.
// FixedKeyV1 is deprecated: its key never changes, so anyone who learns it can
// observe group activity indefinitely and target that one key. It remains
// only for interoperability with older peers; prefer RotatingKeysV2.
enum class DiscoveryProtocol : std::uint8_t { FixedKeyV1, RotatingKeysV2 };

struct WatchOptions {
    // Tracker capacity; lookup and delivery limits are separate and may make
    // discovery incomplete before this many distinct peers are observed. When
    // full, new peers are ignored until tracked advertisements expire; the
    // subscription stays healthy.
    std::size_t max_peers = 128;
    std::size_t max_pending_events = 128;
    std::chrono::milliseconds pulse_interval{500};
    std::uint64_t max_advertisement_lifetime = 300;
    // Wall-clock steps backwards of up to 30 seconds are absorbed by holding
    // the library's time; a larger step reports WatchError::Clock (or the
    // lease/listener equivalent) and the operation must be recreated.
    std::uint64_t clock_skew = 30;
    // Rotating mode requires max_advertisement_lifetime + 2 * clock_skew <= 840 seconds.
    DiscoveryProtocol protocol = DiscoveryProtocol::RotatingKeysV2;
    // Transient mailbox failures retry with exponential backoff; zero retries
    // makes them terminal. Each attempt is bounded and discovery remains best effort.
    std::size_t max_recovery_retries = 5;
    std::chrono::milliseconds recovery_delay{1000};
};

struct DiscoveryHandlers {
    std::function<void(Advertisement, PeerEvent)> on_peer;
    // Lookup completion is not a delivery barrier or proof of network health.
    // Rotating mode can report again after its active window set changes.
    std::function<void(bool)> on_catchup;
    std::function<void(WatchError)> on_error;
    // Optional; notifications can be stale when delivered. Ready reports a
    // recovered watch and catch-up, not completeness; query the subscription
    // for its current state.
    std::function<void(DiscoveryStatus)> on_status{};
};

// Owner-executor only. close and destruction suppress queued events; an event
// already executing may finish. A false healthy() means discovery is incomplete;
// recovering() distinguishes a retry in progress from terminal failure. Verified advertisements
// establish identity, not consent or reachability. DHT discovery is best effort.
// Capabilities are copied in ordinary memory and must be high-entropy secrets.
// The scheduler and executor must outlive pending backend work.
class Subscription {
  public:
    ~Subscription();
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
    void close();
    [[nodiscard]] bool healthy() const;
    [[nodiscard]] bool recovering() const;
    // Checks the exact record against this subscription's verified, current
    // group state. A queued event may already be stale when it is delivered.
    // This does not authorize a peer or prove reachability.
    [[nodiscard]] bool is_current(const Advertisement &value) const;

  private:
    friend class Node;
    friend struct detail::ConnectionAccess;
    explicit Subscription(std::shared_ptr<detail::DiscoveryBackend> state);
    std::shared_ptr<detail::DiscoveryBackend> state_;
};

using WatchResult = std::variant<std::unique_ptr<Subscription>, WatchError>;

enum class AnnouncementError {
    NotReady,
    Unsupported,
    ResourceLimit,
    PublicationLimit,
    Retrying,
    Backend,
    Crypto,
    Clock,
    Scheduler,
    Callback
};

struct AnnounceOptions {
    std::chrono::milliseconds pulse_interval{1000};
    std::uint64_t lifetime_seconds = 120;
    std::uint64_t refresh_seconds = 60;
    std::size_t max_publications = 32;   // 1 through 32 per 120 s window, including retries.
    std::size_t max_pending_events = 32; // 1 through 256.
    DiscoveryProtocol protocol = DiscoveryProtocol::RotatingKeysV2;
    // By default every lease of a node advertises the node's identity (see
    // Node::identity()), so members of different groups, or of one group over
    // time, can recognise the same node. When true, this lease uses signing and
    // recipient keys generated for it alone and never reused. These lease keys
    // are not returned by Node::identity() or exposed directly by Lease. The
    // signaling inbox key is stable for a lease's lifetime in either case.
    bool fresh_identity = false;
};

struct AnnounceHandlers {
    // Backend accepted a publication; this does not mean a peer observed it.
    std::function<void()> on_published;
    // Retrying is nonterminal. Other errors end the lease.
    std::function<void(AnnouncementError)> on_error;
};

// Owner-executor only; the scheduler and executor outlive pending backend work.
// close or destruction suppresses queued events; an event already executing may
// finish. A lease advertises an ephemeral endpoint instance. Dropping it stops refresh
// but cannot retract already published records. The publication limit includes
// retries; it applies per 120-second window in both protocols. Failed puts are
// retried with exponential backoff, and an unpublished record that expires is
// replaced by a fresh one instead of ending the lease. Exhausting the window's
// publication limit is terminal even if the attempts failed.
class Lease {
  public:
    ~Lease();
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    void close();
    [[nodiscard]] bool healthy() const;

  private:
    friend class Node;
    friend struct detail::ConnectionAccess;
    explicit Lease(std::shared_ptr<detail::DiscoveryBackend> state);
    std::shared_ptr<detail::DiscoveryBackend> state_;
};

using AnnounceResult = std::variant<std::unique_ptr<Lease>, AnnouncementError>;
} // namespace p2pdc
