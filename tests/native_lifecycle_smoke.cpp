#include <p2pdc/node.h>
#include <p2pdc/runtime.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

bool rejects_empty_executor(const p2pdc::DiscoveryConfig &config) {
    try {
        static_cast<void>(p2pdc::Node::create(config, {}));
        return false;
    } catch (const std::invalid_argument &) {
        return true;
    }
}

int main() {
    p2pdc::DiscoveryConfig config;
    config.network_id = 873211;
    if (!rejects_empty_executor(config)) {
        std::cerr << "empty executor accepted\n";
        return 1;
    }
    p2pdc::Runtime runtime;
    auto executor = runtime.executor();
    auto node = p2pdc::Node::create(config, executor);
    std::promise<bool> completion;
    auto result = completion.get_future();
    executor([&] {
        const bool accepted = node->start([&](p2pdc::NodeStartResult start) {
            const bool started = start.status == p2pdc::NodeStartStatus::Started;
            const bool duplicate_rejected = !node->start([](auto) {});
            const bool stopping =
                node->shutdown([&, started, duplicate_rejected](p2pdc::NodeStopResult stop) {
                    completion.set_value(started && duplicate_rejected &&
                                         stop.status == p2pdc::NodeStopStatus::Stopped);
                });
            if (!stopping)
                completion.set_value(false);
        });
        if (!accepted)
            completion.set_value(false);
    });
    if (result.wait_for(15s) != std::future_status::ready) {
        std::cerr << "node lifecycle timed out\n";
        std::_Exit(2);
    }
    if (!result.get()) {
        std::cerr << "node lifecycle failed\n";
        return 1;
    }
    std::cout << "native node lifecycle passed\n";
    return 0;
}
