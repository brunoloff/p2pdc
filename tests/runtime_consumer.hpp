#pragma once

#include "p2pdc/runtime.h"
#include "p2pdc/connection.h"
#include "p2pdc/discovery.h"
#include "p2pdc/incoming.h"
#include "p2pdc/node.h"

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace p2pdc_test {
using namespace std::chrono_literals;
using namespace p2pdc;

static_assert(std::is_same_v<DiscoveryConfig, NativeDiscoveryConfig>);
using NodeFactory = std::unique_ptr<Node> (*)(DiscoveryConfig, Executor);
static_assert(std::is_same_v<decltype(&Node::create), NodeFactory>);

template <typename Error, typename Function> bool throws(Function function) {
    try {
        function();
        return false;
    } catch (const Error &) {
        return true;
    }
}

inline bool public_handles_link() {
    auto volatile send = &Connection::send;
    auto volatile route = &Connection::route;
    auto volatile current = &Subscription::is_current;
    auto volatile lease_health = &Lease::healthy;
    auto volatile incoming_state = &IncomingListener::state;
    return send != nullptr && route != nullptr && current != nullptr && lease_health != nullptr &&
           incoming_state != nullptr;
}

inline bool portable_configuration() {
    DiscoveryConfig config;
    config.bootstrap_servers = {{"bootstrap.example", 4222}};
    config.discovery_backends = {p2pdc::OpenDhtProxies{{"https://proxy.example"}}};
    return config.bootstrap_servers.front().host == "bootstrap.example" &&
           config.discovery_backends.front() ==
               p2pdc::DiscoveryBackend(p2pdc::OpenDhtProxies{{"https://proxy.example"}});
}

struct RuntimeConsumer : std::enable_shared_from_this<RuntimeConsumer> {
    explicit RuntimeConsumer(std::function<void(bool)> completion) : done(std::move(completion)) {}
    std::unique_ptr<Runtime> runtime = std::make_unique<Runtime>();
    Executor post = runtime->executor();
    Scheduler schedule = runtime->scheduler();
    std::function<void(bool)> done;
    std::vector<int> order;
    CancelTimer cancel;
    std::weak_ptr<int> canceled_capture;
    bool valid = true;
    bool canceled_ran = false;

    void start();
    void run();
    void finish();
    void check_invalid();
};

inline void RuntimeConsumer::check_invalid() {
    valid = valid && public_handles_link();
    valid = valid && portable_configuration();
    valid = valid && throws<std::invalid_argument>([this] { post({}); });
    valid = valid && throws<std::invalid_argument>([this] { (void)schedule(-1ms, [] {}); });
    valid = valid && throws<std::invalid_argument>([this] { (void)schedule(0ms, {}); });
    valid = valid && throws<std::invalid_argument>(
                         [this] { (void)schedule(std::chrono::milliseconds::max(), [] {}); });
}

inline void RuntimeConsumer::start() {
    auto self = shared_from_this();
    post([self] { self->run(); });
}

inline void RuntimeConsumer::run() {
    check_invalid();
    auto self = shared_from_this();
    post([self] { self->order.push_back(1); });
    post([self] { self->order.push_back(2); });
    valid = valid && order.empty();
    auto capture = std::make_shared<int>(42);
    canceled_capture = capture;
    cancel = schedule(20ms, [self, capture] { self->canceled_ran = true; });
    capture.reset();
    cancel();
    cancel();
    auto distant = schedule(std::chrono::hours(24 * 30), [self] { self->valid = false; });
    distant();
    (void)schedule(100ms, [self] { self->finish(); });
}

inline void RuntimeConsumer::finish() {
    valid = valid && order == std::vector<int>{1, 2} && !canceled_ran && canceled_capture.expired();
    runtime.reset();
    cancel();
    valid = valid && throws<std::runtime_error>([this] { post([] {}); });
    valid = valid && throws<std::runtime_error>([this] { (void)schedule(0ms, [] {}); });
    done(valid);
}

inline void run_runtime_consumer(std::function<void(bool)> done) {
    auto test = std::make_shared<RuntimeConsumer>(std::move(done));
    test->start();
}
} // namespace p2pdc_test
