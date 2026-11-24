#include "native_node_state.hpp"

#ifdef P2PDC_HAVE_PUBLIC_CONNECT
#include "../transport/native_public_connection.hpp"
#include "connection_admission.hpp"
#ifdef __EMSCRIPTEN__
#include "../browser/browser_transport.hpp"
#endif
#include <algorithm>
#endif

namespace p2pdc {
#ifdef P2PDC_HAVE_PUBLIC_CONNECT
namespace {
bool valid_context(detail::AdmissionContext context,
                   const std::weak_ptr<detail::DiscoveryBackend> &local,
                   const std::weak_ptr<detail::DiscoveryBackend> &remote,
                   const detail::AnnouncementEndpoint &endpoint, const Advertisement &peer) {
    auto lease = local.lock();
    auto watch = remote.lock();
    if (!lease || !watch)
        return false;
    auto current = detail::resolve_owned_context(std::move(context), *lease, *watch, peer);
    auto value = std::get_if<detail::ConnectionAdmission>(&current);
    return value && value->local.identity == endpoint.identity &&
           value->local.fields.instance_id == endpoint.fields.instance_id &&
           value->local.fields.signaling_inbox == endpoint.fields.signaling_inbox;
}

bool available(std::vector<std::weak_ptr<detail::ConnectionBackend>> &connections,
               std::size_t limit) {
    std::erase_if(connections, [](const auto &entry) { return entry.expired(); });
    return connections.size() < limit;
}
std::optional<ConnectError> start_outgoing(detail::ConnectionBackend &backend,
                                           const detail::ConnectionAdmission &admission,
                                           IceConfig ice) {
    try {
        return backend.start(admission, std::move(ice));
    } catch (const std::bad_alloc &) {
        return ConnectError::ResourceLimit;
    } catch (const std::exception &) {
        return ConnectError::Backend;
    }
}
} // namespace
#endif
ConnectResult Node::connect(const Lease &lease, const Subscription &subscription,
                            const Advertisement &peer, IceConfig ice, Scheduler scheduler,
                            ConnectOptions options, ConnectionHandlers handlers) {
    auto state = state_;
    if (state->phase != detail::NodePhase::Running) {
        return ConnectError::NotReady;
    }
#ifdef P2PDC_HAVE_PUBLIC_CONNECT
    auto resolved = detail::ConnectionAccess::resolve(*this, lease, subscription, peer);
    if (auto error = std::get_if<detail::AdmissionError>(&resolved)) {
        return *error == detail::AdmissionError::NodeNotReady ? ConnectError::NotReady
                                                              : ConnectError::InvalidPeer;
    }
    if (!available(state->connections, state->max_connections)) {
        return ConnectError::ResourceLimit;
    }
    auto admission = std::get<detail::ConnectionAdmission>(std::move(resolved));
    detail::validate_public_connection_settings(state->executor, scheduler, options, handlers);
    detail::validate_public_ice_settings(ice);
#ifdef __EMSCRIPTEN__
    if (!detail::browser_webrtc_available())
        return ConnectError::Unsupported;
#endif
    auto opposing = state->collisions.opposing(admission, detail::ConnectionDirection::Outgoing);
    if (!opposing.empty() &&
        !detail::CollisionRegistry::preferred(admission, detail::ConnectionDirection::Outgoing))
        return ConnectError::Superseded;
    std::weak_ptr<State> node = state;
    std::weak_ptr<detail::DiscoveryBackend> local = lease.state_, remote = subscription.state_;
    auto endpoint = admission.local;
    auto valid = [node, local, remote, endpoint, peer] {
        auto owner = node.lock();
        if (!owner || owner->phase != detail::NodePhase::Running)
            return false;
#ifdef __EMSCRIPTEN__
        auto context = owner->make_store({32768, 128, 1048576});
#else
        auto context = detail::AdmissionContext{owner->runner, owner->network_backends};
#endif
        return valid_context(std::move(context), local, remote, endpoint, peer);
    };
    auto connection = std::unique_ptr<Connection>(new Connection({}));
    auto backend =
        detail::make_public_connection(state->executor, state->gate, std::move(scheduler), options,
                                       std::move(handlers), std::move(valid));
    connection->state_ = backend;
    state->connections.emplace_back(backend);
    if (auto error = start_outgoing(*backend, admission, std::move(ice))) {
        return *error;
    }
    state->collisions.remember(admission, detail::ConnectionDirection::Outgoing, backend);
    for (auto &other : opposing)
        other->supersede();
    return connection;
#else
    (void)lease;
    (void)subscription;
    (void)peer;
    (void)ice;
    (void)scheduler;
    (void)options;
    (void)handlers;
    return ConnectError::Unsupported;
#endif
}
} // namespace p2pdc
