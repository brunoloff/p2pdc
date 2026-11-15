#include "../src/discovery/native_store.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
constexpr std::array<std::byte, 20> key{std::byte{71}, std::byte{19}};

bool publish(const NativeStore &store, const Bytes &value) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto result = promise->get_future();
    if (store.put(key, value, [promise](bool success) { promise->set_value(success); }))
        return false;
    return result.wait_for(10s) == std::future_status::ready && result.get();
}

bool contains(const NativeStore &store, const Bytes &value) {
    auto promise = std::make_shared<std::promise<LookupResult>>();
    auto result = promise->get_future();
    if (store.lookup(key, [promise](LookupResult item) { promise->set_value(std::move(item)); },
                     LookupDeduplication::Content))
        return false;
    if (result.wait_for(5s) != std::future_status::ready)
        return false;
    auto found = result.get();
    return found.backend_success && !found.batch.error &&
           std::find(found.batch.values.begin(), found.batch.values.end(), value) !=
               found.batch.values.end();
}

bool run(dht::DhtRunner &runner, const NativeStore &store) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (runner.getNodesStats(AF_INET).getKnownNodes() == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    if (!runner.getNodesStats(AF_INET).getKnownNodes() ||
        !publish(store, Bytes{std::byte{0}, std::byte{42}, std::byte{255}}))
        return false;
    std::cout << "READY" << std::endl;
    while (std::chrono::steady_clock::now() < deadline) {
        if (contains(store, Bytes{std::byte{7}})) {
            std::cout << "PASS" << std::endl;
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }
    return false;
}
} // namespace

int main() {
    auto runner = std::make_shared<dht::DhtRunner>();
    dht::DhtRunner::Config config;
    config.dht_config.node_config.network = 873199;
    config.peer_discovery = false;
    config.peer_publish = false;
    runner->run("127.0.0.1", "::1", "0", config);
    runner->bootstrap("127.0.0.1", "49411");
    NativeStore store(runner, {1024, 128, 65536});
    const bool passed = run(*runner, store);
    runner->join();
    if (!passed) std::cerr << "native record peer failed\n";
    return passed ? 0 : 1;
}
