#pragma once

#include "p2pdc/connection.h"
#include "p2pdc/discovery.h"
#include "p2pdc/discovery_config.h"
#include "p2pdc/incoming.h"
#include "p2pdc/operation.h"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace p2pdc {
namespace detail {
struct NodeTestAccess;
}

enum class NodeStartStatus { Started, Canceled, ResourceLimit, BackendFailure };
enum class NodeStopStatus { Stopped, BackendFailure };

struct NodeStartResult {
    NodeStartStatus status;
    std::exception_ptr error;
};

struct NodeStopResult {
    NodeStopStatus status;
    std::exception_ptr error;
};

enum class NodeIdentityError { Unsupported };
struct NodePublicIdentity {
    SigningPublicKey signing_key;
    RecipientPublicKey recipient_key;
    bool operator==(const NodePublicIdentity &) const = default;
};

// Preliminary lifecycle. Native builds require P2PDC_ENABLE_OPENDHT=ON;
// browser builds require libsodium and one proxy URL. Methods and
// callbacks run on the supplied executor, which must be asynchronous, serial,
// FIFO, nonblocking and nonthrowing. It must outlive admitted submissions.
// Startup means the local backend started, not that bootstrap succeeded.
// shutdown cancels pending startup and posts its final callback after earlier
// Node callbacks, runner join, and admitted library backend tasks. This does
// not wait for process-wide WebRTC cleanup. Dropping Node suppresses callbacks not yet
// admitted and requests cleanup without joining. Already running callbacks may
// finish. Callers must poll healthy() for exceptional submission failures; use
// retry_callbacks() to retry locally retained completions. Backend submission
// failure may not be recoverable. Await shutdown before tearing down
// process-wide OpenDHT resources.
class Node {
  public:
    static std::unique_ptr<Node> create(DiscoveryConfig config, Executor executor);
    // Imports both secrets into guarded storage. The caller owns and must wipe
    // the input buffers. Requires discovery and libsodium support.
    [[nodiscard]] static std::variant<std::unique_ptr<Node>, CryptoError, NodeIdentityError>
    create_with_identity(DiscoveryConfig config, Executor executor, ByteView signing_seed,
                         ByteView recipient_secret);
    ~Node();
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    // Returns false if the node has already started or begun shutdown.
    [[nodiscard]] bool start(std::function<void(NodeStartResult)> completion);
    // Returns false if shutdown has already begun.
    [[nodiscard]] bool shutdown(std::function<void(NodeStopResult)> completion);
    [[nodiscard]] bool healthy() const noexcept;
    // Empty until the ephemeral default identity is generated, or after shutdown.
    [[nodiscard]] std::optional<NodePublicIdentity> identity() const noexcept;
    // Retries a locally retained completion; returns false when none is pending.
    [[nodiscard]] bool retry_callbacks();
    // Requires a started node; up to 16 subscriptions may be retained.
    // Callbacks are deferred on the node executor. Scheduler operations must
    // not synchronously reenter this node.
    [[nodiscard]] WatchResult watch(DiscoveryCapability capability, Scheduler scheduler,
                                    WatchOptions options, DiscoveryHandlers handlers);
    // Per node, at most 16 valid announcement attempts are admitted in any
    // rolling five minutes. Fixed-key announcements are also limited to 16 per
    // node lifetime. Leases and subscriptions share a concurrent handle limit.
    // Without imported identity, keys are generated lazily and are ephemeral.
    [[nodiscard]] AnnounceResult announce(DiscoveryCapability capability, Scheduler scheduler,
                                          AnnounceOptions options, AnnounceHandlers handlers);
    // Discovery handles must remain live through initial signaling admission.
    // Once signaling starts, the connection owns its bindings independently.
    // Retained connection handles are limited by DiscoveryConfig::max_connections. Multiple
    // attempts may share a lease; at most one incoming listener per lease may
    // be active. Reopened listeners retain bounded in-memory replay protection.
    // Listeners reopened on a lease require the same discovery lifetime and
    // clock-skew policy as its first listener.
    [[nodiscard]] ConnectResult connect(const Lease &lease, const Subscription &subscription,
                                        const Advertisement &peer, IceConfig ice,
                                        Scheduler scheduler, ConnectOptions options,
                                        ConnectionHandlers handlers);
    [[nodiscard]] ListenResult listen(const Lease &lease, const Subscription &subscription,
                                      Scheduler scheduler, IncomingOptions options,
                                      IncomingHandlers handlers);
    [[nodiscard]] ConnectResult accept(IncomingListener &listener, IceConfig ice,
                                       Scheduler scheduler, ConnectOptions options,
                                       ConnectionHandlers handlers);
    [[nodiscard]] ConnectResult accept(IncomingListener &listener, IncomingRequestId request_id,
                                       IceConfig ice, Scheduler scheduler, ConnectOptions options,
                                       ConnectionHandlers handlers);

  private:
    friend struct detail::ConnectionAccess;
    friend struct detail::NodeTestAccess;
    struct State;
    [[nodiscard]] ConnectResult accept_request(IncomingListener &listener,
                                               std::optional<std::uint64_t> request_id,
                                               IceConfig ice, Scheduler scheduler,
                                               ConnectOptions options, ConnectionHandlers handlers);
    explicit Node(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace p2pdc
