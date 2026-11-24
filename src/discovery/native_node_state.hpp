#pragma once

#include "../core/backend_drain.hpp"
#include "../transport/connection_backend.hpp"
#include "announcement_budget.hpp"
#include "collision_registry.hpp"
#include "incoming_backend.hpp"
#include "native_public_watch.hpp"
#ifndef __EMSCRIPTEN__
#include "native_runner_owner.hpp"
#include "native_runner_start.hpp"
#endif
#ifdef P2PDC_HAVE_NETWORK_BACKENDS
#include "../backends/network_backends.hpp"
#endif
#include "p2pdc/callback_gate.h"
#include "p2pdc/node.h"

#include <optional>
#include <vector>

namespace p2pdc {
namespace detail {
#ifndef P2PDC_HAVE_NETWORK_BACKENDS
class NetworkBackends {};
#endif
enum class NodePhase { Created, Starting, Running, Failed, Stopping, Stopped };
struct IncomingLeaseSlot {
    std::weak_ptr<DiscoveryBackend> lease;
    std::weak_ptr<IncomingBackend> listener;
    std::shared_ptr<IncomingOfferReplay> replay;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
};
} // namespace detail

struct Node::State {
#ifdef __EMSCRIPTEN__
    State(Executor application, std::vector<DiscoveryBackend> backends);
#else
    State(Executor application, detail::RunnerStart starter,
          std::shared_ptr<detail::NetworkBackends> backends);
    void on_started(detail::RunnerStartResult result);
#endif
    ~State();
    void finish_shutdown(std::exception_ptr error, std::weak_ptr<State> weak);
    void begin_backend_drain(std::weak_ptr<State> weak);
    void prepare_start(std::function<void(NodeStartResult)> completion);
    void launch(std::weak_ptr<State> weak);
    void stop_platform(std::function<void(std::exception_ptr)> after_join,
                       std::function<void()> no_runner);
    void release_platform();
    void reset_platform();
    [[nodiscard]] bool platform_healthy() const noexcept;
    void deliver_start();
    void deliver_stop();
    void begin_shutdown(std::function<void(NodeStopResult)> completion, std::weak_ptr<State> weak);
    void close_watches();
    void close_connections();
    void close_incoming();
#ifdef P2PDC_HAVE_DISCOVERY
    [[nodiscard]] std::optional<detail::AnnouncementEndpoint> endpoint_for_lease(bool fresh);
    [[nodiscard]] detail::RecordStore make_store(detail::StoreLimits limits) const;
#endif

    detail::BackendDrain backend_drain;
    Executor executor;
    CallbackGate gate;
#ifdef __EMSCRIPTEN__
    std::vector<DiscoveryBackend> discovery_backends;
#else
    detail::RunnerStart start_runner;
    std::unique_ptr<detail::NativeRunnerOwner> owner;
    std::shared_ptr<dht::DhtRunner> runner;
    // Null when the node meets through the DHT only.
    std::shared_ptr<detail::NetworkBackends> network_backends;
#endif
    std::shared_ptr<const detail::EndpointIdentity> endpoint;
    MemoryBudget watch_budget{4 * 1024 * 1024};
    std::vector<std::weak_ptr<detail::DiscoveryBackend>> watches;
    std::vector<std::weak_ptr<detail::ConnectionBackend>> connections;
    std::size_t max_connections = 8;
    detail::CollisionRegistry collisions;
    std::vector<std::weak_ptr<detail::IncomingBackend>> incoming;
    std::vector<detail::IncomingLeaseSlot> incoming_leases;
    detail::AnnouncementBudget announcement_budget;
    std::optional<CompletionSource<NodeStartResult>> start_completion;
    std::function<void(NodeStopResult)> stop_completion;
    std::optional<NodeStartResult> pending_start;
    std::optional<NodeStopResult> pending_stop;
    bool delivery_failure = false;
    bool delivering_stop = false;
    bool backend_drained = false;
    bool backend_drain_started = false;
    detail::NodePhase phase = detail::NodePhase::Created;
};
} // namespace p2pdc
