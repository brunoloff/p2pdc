#include "native_node_state.hpp"

#include <stdexcept>
#include <utility>

#ifdef P2PDC_HAVE_DISCOVERY
#include "native_store.hpp"
#endif

namespace p2pdc {
using Phase = detail::NodePhase;

Node::State::State(Executor application, detail::RunnerStart starter)
    : backend_drain(application), executor(backend_drain.executor()), gate(std::move(application)),
      start_runner(std::move(starter)) {}
void Node::State::on_started(detail::RunnerStartResult result) {
    if (phase != Phase::Starting) {
        return;
    }
    NodeStartResult outcome{NodeStartStatus::BackendFailure, {}};
    if (auto *ready = std::get_if<std::shared_ptr<dht::DhtRunner>>(&result)) {
        runner = std::move(*ready);
        phase = Phase::Running;
        outcome.status = NodeStartStatus::Started;
    } else if (auto *error = std::get_if<std::exception_ptr>(&result)) {
        phase = Phase::Failed;
        outcome.error = *error;
    } else {
        phase = Phase::Failed;
        outcome.status = NodeStartStatus::Canceled;
    }
    pending_start = std::move(outcome);
    deliver_start();
}

std::unique_ptr<Node> Node::create(DiscoveryConfig config, Executor executor) {
    if (!executor) {
        throw std::invalid_argument("node requires an executor");
    }
    auto start_runner = detail::make_native_runner_start(std::move(config));
    return std::unique_ptr<Node>(
        new Node(std::make_shared<State>(std::move(executor), std::move(start_runner))));
}

void Node::State::launch(std::weak_ptr<State> weak) {
    try {
        owner = detail::NativeRunnerOwner::create(executor, std::move(start_runner),
                                                  [weak](auto result) {
                                                      if (auto locked = weak.lock()) {
                                                          locked->on_started(std::move(result));
                                                      }
                                                  });
    } catch (...) {
        phase = Phase::Failed;
        pending_start = NodeStartResult{NodeStartStatus::BackendFailure, std::current_exception()};
        deliver_start();
        return;
    }
    if (!owner) {
        phase = Phase::Failed;
        pending_start = NodeStartResult{NodeStartStatus::ResourceLimit, {}};
        deliver_start();
    }
}

void Node::State::stop_platform(std::function<void(std::exception_ptr)> after_join,
                                std::function<void()> no_runner) {
    if (owner) {
        if (!owner->shutdown(std::move(after_join)))
            throw std::logic_error("node runner unexpectedly stopping");
    } else {
        executor(std::move(no_runner));
    }
}
void Node::State::release_platform() { owner.reset(); }
void Node::State::reset_platform() { runner.reset(); }
bool Node::State::platform_healthy() const noexcept { return !owner || !owner->delivery_failed(); }
#ifdef P2PDC_HAVE_DISCOVERY
detail::RecordStore Node::State::make_store(detail::StoreLimits limits) const {
    return detail::NativeStore(runner, limits);
}
#endif
} // namespace p2pdc
