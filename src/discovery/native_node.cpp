#include "p2pdc/node.h"

#include "native_node_state.hpp"

#ifdef P2PDC_HAVE_DISCOVERY
#include "connection_admission.hpp"
#endif

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace p2pdc {
using Phase = detail::NodePhase;

Node::State::~State() {
    gate.abandon();
    close_incoming();
    close_connections();
    close_watches();
    endpoint.reset();
    if (start_completion) {
        start_completion->abandon();
    }
    release_platform();
}

void Node::State::close_incoming() {
    for (auto &listener : incoming) {
        if (auto active = listener.lock())
            active->close();
    }
    incoming.clear();
}

void Node::State::close_connections() {
    for (auto &connection : connections) {
        if (auto active = connection.lock()) {
            active->close();
        }
    }
    connections.clear();
}

void Node::State::close_watches() {
    for (auto &watch : watches) {
        if (auto active = watch.lock()) {
            active->close();
        }
    }
    watches.clear();
}

#ifdef P2PDC_HAVE_DISCOVERY
std::optional<detail::AnnouncementEndpoint> Node::State::endpoint_for_lease(bool fresh) {
    if (fresh || !endpoint) {
        auto identity = detail::make_endpoint_identity();
        if (std::holds_alternative<CryptoError>(identity)) {
            return std::nullopt;
        }
        auto generated =
            std::get<std::shared_ptr<const detail::EndpointIdentity>>(std::move(identity));
        if (fresh)
            return detail::make_announcement_endpoint(std::move(generated));
        endpoint = std::move(generated);
    }
    return detail::make_announcement_endpoint(endpoint);
}
#endif

void Node::State::finish_shutdown(std::exception_ptr error, std::weak_ptr<State> weak) {
    if (phase != Phase::Stopping) {
        return;
    }
    pending_stop =
        NodeStopResult{error ? NodeStopStatus::BackendFailure : NodeStopStatus::Stopped, error};
    try {
        begin_backend_drain(std::move(weak));
    } catch (...) {
        delivery_failure = true;
        throw;
    }
}

void Node::State::begin_backend_drain(std::weak_ptr<State> weak) {
    if (!backend_drain.begin([weak] {
            if (auto state = weak.lock()) {
                state->backend_drained = true;
                state->deliver_stop();
            }
        }))
        throw std::logic_error("node backend drain already started");
    backend_drain_started = true;
}

void Node::State::deliver_start() {
    if (!pending_start) {
        return;
    }
    try {
        if (!start_completion->complete(*pending_start)) {
            throw std::logic_error("node startup completion already delivered");
        }
        if (pending_start->status == NodeStartStatus::Canceled && phase == Phase::Starting) {
            phase = Phase::Failed;
        }
        pending_start.reset();
        if (!pending_stop) {
            delivery_failure = false;
        }
    } catch (...) {
        delivery_failure = true;
        throw;
    }
}

void Node::State::deliver_stop() {
    if (!pending_stop || delivering_stop) {
        return;
    }
    delivering_stop = true;
    struct ResetDelivery {
        bool &active;
        ~ResetDelivery() { active = false; }
    } reset{delivering_stop};
    try {
        auto callback = stop_completion;
        auto result = *pending_stop;
        if (!gate.close([callback = std::move(callback), result]() mutable { callback(result); })) {
            throw std::logic_error("node callback gate unexpectedly closed");
        }
        phase = Phase::Stopped;
        pending_stop.reset();
        stop_completion = {};
        delivery_failure = false;
    } catch (...) {
        delivery_failure = true;
        throw;
    }
}

Node::Node(std::shared_ptr<State> state) : state_(std::move(state)) {}
Node::~Node() = default;

void Node::State::prepare_start(std::function<void(NodeStartResult)> completion) {
    auto callback_gate = gate;
    Executor guarded = [callback_gate](auto task) {
        if (!callback_gate.post(std::move(task))) {
            throw std::logic_error("node callback gate unexpectedly closed");
        }
    };
    start_completion.emplace(std::move(guarded), [completion = std::move(completion)](
                                                     OperationResult<NodeStartResult> value) {
        completion(std::get<NodeStartResult>(std::move(value)));
    });
    phase = Phase::Starting;
}

bool Node::start(std::function<void(NodeStartResult)> completion) {
    if (!completion) {
        throw std::invalid_argument("node startup requires a completion callback");
    }
    auto state = state_;
    if (state->phase != Phase::Created) {
        return false;
    }
    state->prepare_start(std::move(completion));
    state->launch(state);
    return true;
}

void Node::State::begin_shutdown(std::function<void(NodeStopResult)> completion,
                                 std::weak_ptr<State> weak) {
    std::function<void(std::exception_ptr)> after_join = [weak](std::exception_ptr error) {
        if (auto locked = weak.lock()) {
            locked->finish_shutdown(error, weak);
        }
    };
    std::function<void()> no_runner = [weak] {
        if (auto locked = weak.lock()) {
            locked->finish_shutdown({}, weak);
        }
    };
    const auto previous = phase;
    if (phase == Phase::Starting) {
        pending_start = NodeStartResult{NodeStartStatus::Canceled, {}};
    }
    deliver_start();
    phase = Phase::Stopping;
    stop_completion = std::move(completion);
    try {
        close_incoming();
        close_connections();
        close_watches();
        stop_platform(std::move(after_join), std::move(no_runner));
    } catch (...) {
        phase = previous == Phase::Starting ? Phase::Failed : previous;
        stop_completion = {};
        throw;
    }
    reset_platform();
    endpoint.reset();
}

bool Node::shutdown(std::function<void(NodeStopResult)> completion) {
    if (!completion) {
        throw std::invalid_argument("node shutdown requires a completion callback");
    }
    auto state = state_;
    if (state->phase == Phase::Stopping || state->phase == Phase::Stopped) {
        return false;
    }
    state->begin_shutdown(std::move(completion), state);
    return true;
}

bool Node::healthy() const noexcept {
    return !state_->delivery_failure && state_->platform_healthy();
}

bool Node::retry_callbacks() {
    auto state = state_;
    if (state->delivering_stop || (!state->pending_start && !state->pending_stop)) {
        return false;
    }
    state->deliver_start();
    if (state->pending_stop && !state->backend_drained) {
        if (state->backend_drain_started)
            return false;
        state->begin_backend_drain(state);
    }
    if (state->backend_drained)
        state->deliver_stop();
    state->delivery_failure = false;
    return true;
}

WatchResult Node::watch(DiscoveryCapability capability, Scheduler scheduler, WatchOptions options,
                        DiscoveryHandlers handlers) {
    auto state = state_;
    if (state->phase != Phase::Running) {
        return WatchError::NotReady;
    }
#ifdef P2PDC_HAVE_DISCOVERY
    std::erase_if(state->watches, [](const auto &watch) { return watch.expired(); });
    if (state->watches.size() >= 16) {
        return WatchError::ResourceLimit;
    }
    detail::validate_watch(scheduler, options, handlers);
    auto subscription = std::unique_ptr<Subscription>(new Subscription({}));
    const auto records = std::clamp(options.max_peers * 4, std::size_t{64}, std::size_t{1024});
    auto store = state->make_store({1024, records, records * 1024});
    auto result = detail::make_public_watch(store, capability, std::move(scheduler), options,
                                            std::move(handlers), state->executor, state->gate,
                                            state->watch_budget);
    if (auto *error = std::get_if<WatchError>(&result)) {
        return *error;
    }
    auto backend = std::get<std::shared_ptr<detail::DiscoveryBackend>>(std::move(result));
    if (state->phase != Phase::Running) {
        backend->close();
        return WatchError::NotReady;
    }
    subscription->state_ = backend;
    state->watches.emplace_back(backend);
    return subscription;
#else
    (void)capability;
    (void)scheduler;
    (void)options;
    (void)handlers;
    return WatchError::Unsupported;
#endif
}

AnnounceResult Node::announce(DiscoveryCapability capability, Scheduler scheduler,
                              AnnounceOptions options, AnnounceHandlers handlers) {
    auto state = state_;
    if (state->phase != Phase::Running) {
        return AnnouncementError::NotReady;
    }
#ifdef P2PDC_HAVE_DISCOVERY
    detail::validate_announcement(scheduler, options, handlers);
    std::erase_if(state->watches, [](const auto &handle) { return handle.expired(); });
    if (state->watches.size() >= 16) {
        return AnnouncementError::ResourceLimit;
    }
    if (!state->announcement_budget.admit(detail::AnnouncementBudget::Clock::now(),
                                          options.protocol))
        return AnnouncementError::ResourceLimit;
    auto lease = std::unique_ptr<Lease>(new Lease({}));
    auto endpoint = state->endpoint_for_lease(options.fresh_identity);
    if (!endpoint) {
        return AnnouncementError::Crypto;
    }
    auto store = state->make_store({1024, 64, 65536});
    auto result = detail::make_public_lease(store, std::move(*endpoint), capability,
                                            std::move(scheduler), options, std::move(handlers),
                                            state->executor, state->gate, state->watch_budget);
    if (auto *error = std::get_if<AnnouncementError>(&result)) {
        return *error;
    }
    auto backend = std::get<std::shared_ptr<detail::DiscoveryBackend>>(std::move(result));
    if (state->phase != Phase::Running) {
        backend->close();
        return AnnouncementError::NotReady;
    }
    lease->state_ = backend;
    state->watches.emplace_back(backend);
    return lease;
#else
    (void)capability;
    (void)scheduler;
    (void)options;
    (void)handlers;
    return AnnouncementError::Unsupported;
#endif
}

#ifdef P2PDC_HAVE_DISCOVERY
detail::AdmissionResult detail::ConnectionAccess::resolve(Node &node, const Lease &lease,
                                                          const Subscription &subscription,
                                                          const Advertisement &peer) {
    auto state = node.state_;
    if (state->phase != Phase::Running || !node.healthy()) {
        return AdmissionError::NodeNotReady;
    }
    const auto owned = [&](const auto &backend) {
        return backend && std::any_of(state->watches.begin(), state->watches.end(),
                                      [&](const auto &entry) { return entry.lock() == backend; });
    };
    if (!owned(lease.state_) || !owned(subscription.state_)) {
        return AdmissionError::ForeignHandle;
    }
#ifdef __EMSCRIPTEN__
    return resolve_owned_context(state->make_store({32768, 128, 1048576}), *lease.state_,
                                 *subscription.state_, peer);
#else
    return resolve_owned_context(AdmissionContext{state->runner, state->network_backends},
                                 *lease.state_, *subscription.state_, peer);
#endif
}
#endif
} // namespace p2pdc
