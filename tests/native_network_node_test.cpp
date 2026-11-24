#include "backend_servers.hpp"
#include "echo_example_pair.hpp"
#include "p2pdc/runtime.h"

#include <gtest/gtest.h>

#include <future>

using namespace p2pdc;
using namespace std::chrono_literals;

namespace {
// Two native nodes, each alone in a DHT of its own (no bootstrap servers), so that they can
// meet only through the configured backends: they find each other, connect and echo.
std::pair<bool, std::string> echo_through(std::vector<DiscoveryBackend> backends) {
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    auto future = result->get_future();
    DiscoveryConfig config;
    config.network_id = 0x5ee7ab1e;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.discovery_backends = std::move(backends);
    executor([config, executor, scheduler, result] {
        (void)p2pdc_test::run_echo_example_pair(
            config, executor, scheduler, {}, false, false,
            [result](bool success, std::string reason) { result->set_value({success, reason}); });
    });
    if (future.wait_for(45s) != std::future_status::ready)
        return {false, "no result"};
    return future.get();
}
} // namespace

TEST(NativeNetworkNode, NodesMeetAndConnectThroughANostrRelay) {
    p2pdc::test::BackendServers servers;
    const auto [success, reason] = echo_through({NostrRelay{servers.nostr}});
    EXPECT_TRUE(success) << reason;
    EXPECT_GT(servers.command("stats")["nostrEvents"], 0);
    EXPECT_EQ(servers.command("stats")["nostrInvalid"], 0);
}

TEST(NativeNetworkNode, NodesMeetAndConnectThroughAnMqttBroker) {
    p2pdc::test::BackendServers servers;
    const auto [success, reason] = echo_through({MqttBroker{servers.mqtt}});
    EXPECT_TRUE(success) << reason;
    EXPECT_GT(servers.command("stats")["mqttRetained"], 0);
}

TEST(NativeNetworkNode, NodesMeetThroughAnyBackendTheyShare) {
    p2pdc::test::BackendServers servers;
    // An unreachable relay beside a working broker: the fan-out store carries on without it.
    const auto [success, reason] =
        echo_through({NostrRelay{"ws://127.0.0.1:9"}, MqttBroker{servers.mqtt},
                      OpenDhtProxies{{"https://proxy.example"}}});
    EXPECT_TRUE(success) << reason;
}

// The control: without a shared backend the isolated DHTs keep the nodes apart.
TEST(NativeNetworkNode, NodesInSeparateDhtsDoNotMeetWithoutABackend) {
    const auto [success, reason] = echo_through({});
    EXPECT_FALSE(success);
    EXPECT_EQ(reason, "echo timed out");
}

TEST(NativeNetworkNode, RefusesAnInvalidBackendList) {
    Runtime runtime;
    DiscoveryConfig config;
    config.discovery_backends = {NostrRelay{"https://relay.example"}};
    EXPECT_THROW(static_cast<void>(Node::create(config, runtime.executor())),
                 std::invalid_argument);
    config.discovery_backends = std::vector<DiscoveryBackend>(9, NostrRelay{"wss://relay.example"});
    EXPECT_THROW(static_cast<void>(Node::create(config, runtime.executor())),
                 std::invalid_argument);
}
