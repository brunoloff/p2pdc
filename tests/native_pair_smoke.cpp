#include "node_connection_consumer.hpp"
#include "node_race_consumer.hpp"
#include <p2pdc/runtime.h>

#include <opendht.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string_view>

using namespace std::chrono_literals;

bool bootstrap(dht::DhtRunner &first, dht::DhtRunner &second) {
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    second.bootstrap(first.getBound(), [completion](bool ok) { completion->set_value(ok); });
    return result.wait_for(10s) == std::future_status::ready && result.get();
}

bool exchange(dht::DhtRunner &first) {
    p2pdc::Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    p2pdc::DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = p2pdc::NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first.getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, completion] {
        (void)p2pdc_test::run_node_connection_consumer(
            config, executor, scheduler, {}, false,
            [completion](bool success) { completion->set_value(success); });
    });
    if (result.wait_for(30s) != std::future_status::ready) {
        std::cerr << "public node pair timed out\n";
        std::_Exit(2);
    }
    return result.get();
}

bool race(dht::DhtRunner &first, p2pdc_test::RaceMode mode) {
    p2pdc::Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    p2pdc::DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = p2pdc::NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first.getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, completion, mode] {
        (void)p2pdc_test::run_node_race_consumer(
            config, executor, scheduler, mode,
            [completion](bool success) { completion->set_value(success); });
    });
    if (result.wait_for(40s) != std::future_status::ready) {
        std::cerr << "public node race timed out\n";
        std::_Exit(2);
    }
    return result.get();
}

bool races(dht::DhtRunner &first) {
    const std::array names{"collision", "cancel", "reject", "cancel-accepted", "persistent"};
    std::size_t index = 0;
    for (auto mode : {p2pdc_test::RaceMode::Collision, p2pdc_test::RaceMode::Cancel,
                      p2pdc_test::RaceMode::Reject, p2pdc_test::RaceMode::CancelAccepted,
                      p2pdc_test::RaceMode::Persistent}) {
        std::cerr << "Android race: " << names[index++] << '\n';
        if (!race(first, mode))
            return false;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--races")) {
        std::cerr << "usage: public node pair [--races]\n";
        return 2;
    }
    try {
        dht::DhtRunner first, second;
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345683;
        config.peer_discovery = false;
        config.peer_publish = false;
        first.run("127.0.0.1", "::1", "0", config);
        second.run("127.0.0.1", "::1", "0", config);
        bool success = bootstrap(first, second) && (argc == 2 ? races(first) : exchange(first));
        first.join();
        second.join();
        if (!success) {
            std::cerr << "public node pair failed\n";
            return 1;
        }
        std::cout << "public node pair passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "public node pair: " << error.what() << '\n';
        return 1;
    }
}
