#include "../src/discovery/native_runner_start.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

TEST(NativeRunnerStart, AcceptsEmptyAndBareIpBootstrapConfiguration) {
    EXPECT_TRUE(make_native_runner_start({}));
    NativeDiscoveryConfig config;
    config.bootstrap_servers = {{"127.0.0.1", 4222}, {"::1", 4222}, {"example.org", 1234}};
    EXPECT_TRUE(make_native_runner_start(config));
}

TEST(NativeRunnerStart, SharedConfigurationPreservesNativeValidation) {
    DiscoveryConfig config;
    config.bootstrap_servers = {{"127.0.0.1", 4222}};
    config.browser_backends = {p2pdc::OpenDhtProxies{{"https://proxy.example"}}};
    EXPECT_TRUE(make_native_runner_start(config));
    config.bootstrap_servers = {{"invalid host", 4222}};
    EXPECT_THROW(static_cast<void>(make_native_runner_start(config)), std::invalid_argument);
}

TEST(NativeRunnerStart, RejectsMalformedOrExcessiveBootstrapServers) {
    NativeDiscoveryConfig config;
    for (const std::string &host :
         {std::string{}, std::string{"https://example.org"}, std::string{"host name"},
          std::string{"[::1]"}, std::string{"example.org:4222"}, std::string{":::"},
          std::string{"a\0b", 3}}) {
        config.bootstrap_servers = {{host, 4222}};
        EXPECT_THROW(static_cast<void>(make_native_runner_start(config)), std::invalid_argument);
    }
    config.bootstrap_servers = {{"example.org", 0}};
    EXPECT_THROW(static_cast<void>(make_native_runner_start(config)), std::invalid_argument);
    config.bootstrap_servers = {{std::string(254, 'a'), 4222}};
    EXPECT_THROW(static_cast<void>(make_native_runner_start(config)), std::invalid_argument);
    config.bootstrap_servers.assign(33, {"example.org", 4222});
    EXPECT_THROW(static_cast<void>(make_native_runner_start(config)), std::invalid_argument);
    config.bootstrap_servers.clear();
    config.bind_addresses = NativeBindAddresses{"", "::1"};
    EXPECT_THROW(static_cast<void>(make_native_runner_start(config)), std::invalid_argument);
}

TEST(NativeRunnerStart, BootstrapsLocalNodeUsingPublicConfiguration) {
    dht::DhtRunner first;
    dht::DhtRunner::Config options;
    options.dht_config.node_config.network = 0x12345693;
    options.peer_discovery = false;
    options.peer_publish = false;
    first.run("127.0.0.1", "::1", "0", options);
    NativeDiscoveryConfig config;
    config.network_id = 0x12345693;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first.getBoundPort(AF_INET)}};
    config.browser_backends = {p2pdc::OpenDhtProxies{{"https://proxy.example"}}};
    dht::DhtRunner second;
    make_native_runner_start(config)(second);
    ASSERT_TRUE(second.isRunning());
    EXPECT_TRUE(second.getBound(AF_INET).isLoopback());
    EXPECT_TRUE(second.getBound(AF_INET6).isLoopback());
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (second.getNodesStats(AF_INET).getKnownNodes() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_GT(second.getNodesStats(AF_INET).getKnownNodes(), 0U);
    const auto key = dht::InfoHash::get("p2pdc-bootstrap-config-test");
    const dht::Blob expected{0, 42, 255};
    auto published = std::make_shared<std::promise<bool>>();
    auto publication = published->get_future();
    first.put(key, dht::Value(expected), [published](bool ok) { published->set_value(ok); });
    ASSERT_EQ(publication.wait_for(15s), std::future_status::ready);
    ASSERT_TRUE(publication.get());
    auto lookup = second.get(key);
    ASSERT_EQ(lookup.wait_for(15s), std::future_status::ready);
    const auto records = lookup.get();
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.front()->data, expected);
    second.join();
    first.join();
}
