#include "../discovery/native_node_state.hpp"
#include "browser_record_store.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc {
using Phase = detail::NodePhase;

Node::State::State(Executor application, std::vector<DiscoveryBackend> backends)
    : backend_drain(application), executor(backend_drain.executor()), gate(std::move(application)),
      discovery_backends(std::move(backends)) {}

std::unique_ptr<Node> Node::create(DiscoveryConfig config, Executor executor) {
    if (!executor)
        throw std::invalid_argument("node requires an executor");
    if (config.max_connections == 0 || config.max_connections > 1024)
        throw std::invalid_argument("max_connections must be between 1 and 1024");
    // Checks the backends now; the store itself is dropped.
    (void)detail::make_browser_record_store(config.discovery_backends, {1024, 64, 65536});
    auto state = std::make_shared<State>(std::move(executor), std::move(config.discovery_backends));
    state->max_connections = config.max_connections;
    return std::unique_ptr<Node>(new Node(std::move(state)));
}

void Node::State::launch(std::weak_ptr<State> weak) {
    try {
        executor([weak] {
            if (auto state = weak.lock(); state && state->phase == Phase::Starting) {
                state->phase = Phase::Running;
                state->pending_start = NodeStartResult{NodeStartStatus::Started, {}};
                state->deliver_start();
            }
        });
    } catch (...) {
        phase = Phase::Failed;
        pending_start = NodeStartResult{NodeStartStatus::BackendFailure, std::current_exception()};
        deliver_start();
    }
}
void Node::State::stop_platform(std::function<void(std::exception_ptr)>,
                                std::function<void()> stopped) {
    executor(std::move(stopped));
}
void Node::State::release_platform() {}
void Node::State::reset_platform() {}
bool Node::State::platform_healthy() const noexcept { return true; }
detail::RecordStore Node::State::make_store(detail::StoreLimits limits) const {
    return detail::make_browser_record_store(discovery_backends, limits);
}
} // namespace p2pdc
