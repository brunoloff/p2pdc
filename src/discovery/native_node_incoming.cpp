#include "native_node_state.hpp"

#ifdef P2PDC_HAVE_INCOMING_LISTENER
#include "native_public_incoming.hpp"
#include <algorithm>
#include <sodium.h>
#endif
#ifdef P2PDC_HAVE_PUBLIC_CONNECT
#include "../transport/attempt_binding.hpp"
#include "../transport/native_public_connection.hpp"
#include "connection_admission.hpp"
#ifdef __EMSCRIPTEN__
#include "../browser/browser_transport.hpp"
#endif
#endif

namespace p2pdc {
#ifdef P2PDC_HAVE_INCOMING_LISTENER
namespace {
template <class Backend>
bool contains(const std::vector<std::weak_ptr<Backend>> &entries,
              const std::shared_ptr<Backend> &value) {
    return value && std::any_of(entries.begin(), entries.end(),
                                [&](const auto &entry) { return entry.lock() == value; });
}
bool same_group(const detail::IncomingContext &context) {
    auto local = context.local.capability;
    auto remote = context.remote.capability;
    return context.local.protocol == context.remote.protocol && local && remote &&
           local->size() == capability_size && remote->size() == capability_size &&
           sodium_memcmp(local->data(), remote->data(), capability_size) == 0;
}
std::optional<IncomingError> check_slot(const detail::IncomingLeaseSlot &slot,
                                        const detail::PeerCollection &remote) {
    if (slot.max_lifetime != remote.max_lifetime || slot.clock_skew != remote.clock_skew)
        return IncomingError::InvalidContext;
    auto listener = slot.listener.lock();
    if (listener && !listener->signaling_closed())
        return IncomingError::ResourceLimit;
    return std::nullopt;
}
template <class State>
std::variant<detail::IncomingLeaseSlot *, IncomingError>
find_slot(State &state, const std::shared_ptr<detail::DiscoveryBackend> &lease,
          const detail::PeerCollection &remote) {
    std::erase_if(state.incoming, [](const auto &item) { return item.expired(); });
    std::erase_if(state.incoming_leases, [](const auto &item) { return item.lease.expired(); });
    if (state.incoming.size() >= 8)
        return IncomingError::ResourceLimit;
    auto found = std::find_if(state.incoming_leases.begin(), state.incoming_leases.end(),
                              [&](const auto &item) { return item.lease.lock() == lease; });
    if (found == state.incoming_leases.end())
        return static_cast<detail::IncomingLeaseSlot *>(nullptr);
    if (auto error = check_slot(*found, remote))
        return *error;
    return &*found;
}
template <class State>
void remember_listener(State &state, detail::IncomingLeaseSlot *slot,
                       const std::shared_ptr<detail::DiscoveryBackend> &lease,
                       const std::shared_ptr<detail::IncomingBackend> &listener,
                       std::shared_ptr<detail::IncomingOfferReplay> replay, std::uint64_t lifetime,
                       std::uint64_t skew) {
    state.incoming.emplace_back(listener);
    if (slot)
        slot->listener = listener;
    else
        state.incoming_leases.push_back({lease, listener, std::move(replay), lifetime, skew});
}
#ifdef P2PDC_HAVE_PUBLIC_CONNECT
bool same_binding(const detail::ConnectionAdmission &value,
                  const detail::PendingIncoming &pending) {
    return value.local.identity && value.local.identity->signing &&
           value.local.identity->recipient &&
           value.local.identity->signing->public_key() == pending.signing_key &&
           value.local.identity->recipient->public_key() == pending.box_key &&
           value.local.fields.instance_id == pending.endpoint.fields.instance_id &&
           value.local.fields.signaling_inbox == pending.endpoint.fields.signaling_inbox;
}
#endif
template <class State>
auto context_provider(std::weak_ptr<State> node, std::weak_ptr<detail::DiscoveryBackend> local,
                      std::weak_ptr<detail::DiscoveryBackend> remote) {
    return [node, local, remote]() -> std::optional<detail::IncomingContext> {
        auto owner = node.lock();
        auto lease = local.lock();
        auto watch = remote.lock();
        if (!owner || owner->phase != detail::NodePhase::Running ||
            !contains(owner->watches, lease) || !contains(owner->watches, watch))
            return std::nullopt;
        auto endpoint = lease->lease_snapshot();
        auto peers = watch->current_peers();
        if (!endpoint || !peers)
            return std::nullopt;
        return detail::IncomingContext{std::move(*endpoint), std::move(*peers)};
    };
}
#ifdef P2PDC_HAVE_PUBLIC_CONNECT
template <class State>
auto admission_validator(std::weak_ptr<State> node, detail::PendingIncoming pending) {
    return [node, pending = std::move(pending)] {
        auto owner = node.lock();
        auto local = pending.lease.lock();
        auto remote = pending.subscription.lock();
        if (!owner || owner->phase != detail::NodePhase::Running ||
            !contains(owner->watches, local) || !contains(owner->watches, remote))
            return false;
#ifdef __EMSCRIPTEN__
        auto context = owner->make_store({32768, 128, 1048576});
#else
        auto context = owner->runner;
#endif
        auto result =
            detail::resolve_owned_context(std::move(context), *local, *remote, pending.offer.peer);
        auto admission = std::get_if<detail::ConnectionAdmission>(&result);
        return admission && same_binding(*admission, pending);
    };
}
std::optional<ConnectError> start_accepted(detail::ConnectionBackend &backend,
                                           const detail::ConnectionAdmission &admission,
                                           IceConfig ice, const detail::IncomingOffer &offer) {
    try {
        return backend.start(admission, std::move(ice), &offer);
    } catch (const detail::IncomingOfferInvalid &) {
        return ConnectError::InvalidPeer;
    } catch (const detail::IncomingCryptoFailure &error) {
        return error.error() == CryptoError::AllocationFailed ? ConnectError::ResourceLimit
                                                              : ConnectError::Backend;
    } catch (const std::bad_alloc &) {
        return ConnectError::ResourceLimit;
    } catch (const std::exception &) {
        return ConnectError::Backend;
    }
}
#endif
} // namespace

ListenResult Node::listen(const Lease &lease, const Subscription &subscription, Scheduler scheduler,
                          IncomingOptions options, IncomingHandlers handlers) {
    auto state = state_;
    if (state->phase != detail::NodePhase::Running || !healthy())
        return IncomingError::NotReady;
    if (!contains(state->watches, lease.state_) || !contains(state->watches, subscription.state_))
        return IncomingError::InvalidContext;
    auto local = lease.state_->lease_snapshot();
    auto remote = subscription.state_->current_peers();
    if (!local || !remote)
        return IncomingError::InvalidContext;
    detail::IncomingContext initial{std::move(*local), std::move(*remote)};
    if (!same_group(initial) || !initial.local.endpoint.identity ||
        !initial.local.endpoint.identity->signing || !initial.local.endpoint.identity->recipient)
        return IncomingError::InvalidContext;
    auto selected = find_slot(*state, lease.state_, initial.remote);
    if (auto error = std::get_if<IncomingError>(&selected))
        return *error;
    auto *slot = std::get<detail::IncomingLeaseSlot *>(selected);
    auto replay = slot ? slot->replay : std::make_shared<detail::IncomingOfferReplay>();
    const auto max_lifetime = initial.remote.max_lifetime, clock_skew = initial.remote.clock_skew;
    std::weak_ptr<State> node = state;
    std::weak_ptr<detail::DiscoveryBackend> local_handle = lease.state_,
                                            remote_handle = subscription.state_;
    auto context = context_provider(node, local_handle, remote_handle);
    auto handle = std::unique_ptr<IncomingListener>(new IncomingListener({}));
    auto backend = detail::make_public_incoming(
        state->make_store({32768, 128, 1048576}), std::move(initial), std::move(context),
        local_handle, remote_handle, state->executor, state->gate, std::move(scheduler), options,
        std::move(handlers), replay);
    handle->state_ = backend;
    remember_listener(*state, slot, lease.state_, backend, std::move(replay), max_lifetime,
                      clock_skew);
    if (auto error = backend->start())
        return *error;
    return handle;
}

#else
ListenResult Node::listen(const Lease &, const Subscription &, Scheduler, IncomingOptions,
                          IncomingHandlers) {
    return state_->phase == detail::NodePhase::Running && healthy() ? IncomingError::Unsupported
                                                                    : IncomingError::NotReady;
}
#endif

#ifdef P2PDC_HAVE_PUBLIC_CONNECT
ConnectResult Node::accept(IncomingListener &listener, IceConfig ice, Scheduler scheduler,
                           ConnectOptions options, ConnectionHandlers handlers) {
    auto state = state_;
    if (state->phase != detail::NodePhase::Running || !healthy())
        return ConnectError::NotReady;
    if (!contains(state->incoming, listener.state_))
        return ConnectError::InvalidPeer;
    std::erase_if(state->connections, [](const auto &entry) { return entry.expired(); });
    if (state->connections.size() >= 8)
        return ConnectError::ResourceLimit;
    detail::validate_public_connection_settings(state->executor, scheduler, options, handlers);
    detail::validate_public_ice_settings(ice);
    auto pending = listener.state_->peek();
    if (!pending)
        return ConnectError::InvalidPeer;
    auto local = pending->lease.lock();
    auto remote = pending->subscription.lock();
    if (!contains(state->watches, local) || !contains(state->watches, remote))
        return ConnectError::InvalidPeer;
#ifdef __EMSCRIPTEN__
    auto context = state->make_store({32768, 128, 1048576});
#else
    auto context = state->runner;
#endif
    auto result =
        detail::resolve_owned_context(std::move(context), *local, *remote, pending->offer.peer);
    auto admission = std::get_if<detail::ConnectionAdmission>(&result);
    if (!admission || !same_binding(*admission, *pending))
        return ConnectError::InvalidPeer;
#ifdef __EMSCRIPTEN__
    if (!detail::browser_webrtc_available())
        return ConnectError::Unsupported;
#endif
    auto opposing = state->collisions.opposing(*admission, detail::ConnectionDirection::Incoming);
    if (!opposing.empty() &&
        !detail::CollisionRegistry::preferred(*admission, detail::ConnectionDirection::Incoming)) {
        listener.close();
        return ConnectError::Superseded;
    }
    pending = listener.state_->take();
    if (!pending)
        return ConnectError::InvalidPeer;
    std::weak_ptr<State> node = state;
    auto valid = admission_validator(node, *pending);
    auto connection = std::unique_ptr<Connection>(new Connection({}));
    auto backend =
        detail::make_public_connection(state->executor, state->gate, std::move(scheduler), options,
                                       std::move(handlers), std::move(valid));
    connection->state_ = backend;
    state->connections.emplace_back(backend);
    if (auto error = start_accepted(*backend, *admission, std::move(ice), pending->offer))
        return *error;
    state->collisions.remember(*admission, detail::ConnectionDirection::Incoming, backend);
    for (auto &other : opposing)
        other->supersede();
    return connection;
}
#else
ConnectResult Node::accept(IncomingListener &, IceConfig, Scheduler, ConnectOptions,
                           ConnectionHandlers) {
    if (state_->phase != detail::NodePhase::Running || !healthy())
        return ConnectError::NotReady;
    return ConnectError::Unsupported;
}
#endif
} // namespace p2pdc
