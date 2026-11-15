#include "echo_example_pair.hpp"
#include "native_attempt_fixture.hpp"
#include "p2pdc/runtime.h"

namespace {
class NativeEchoExampleTest : public NativeAttemptTest {};

TEST_F(NativeEchoExampleTest, SharedExampleExchangesAndReportsDirectRoute) {
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    auto future = result->get_future();
    DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, result] {
        (void)p2pdc_test::run_echo_example_pair(
            config, executor, scheduler, {}, false, false,
            [result](bool success, std::string reason) { result->set_value({success, reason}); });
    });
    ASSERT_EQ(future.wait_for(40s), std::future_status::ready);
    auto [success, reason] = future.get();
    EXPECT_TRUE(success) << reason;
}

TEST_F(NativeEchoExampleTest, RejectsAnUnauthorizedIncomingPeer) {
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    auto future = result->get_future();
    DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, result] {
        (void)p2pdc_test::run_echo_example_pair(
            config, executor, scheduler, {}, false, true,
            [result](bool success, std::string reason) { result->set_value({success, reason}); });
    });
    ASSERT_EQ(future.wait_for(40s), std::future_status::ready);
    auto [success, reason] = future.get();
    EXPECT_FALSE(success);
    EXPECT_EQ(reason, "incoming peer is not authorized or no longer current");
}

TEST_F(NativeEchoExampleTest, CancelDuringStartupReportsOnceAndShutsDown) {
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    auto future = result->get_future();
    DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, result] {
        auto cancel = p2pdc_test::run_echo_example_pair(
            config, executor, scheduler, {}, false, false,
            [result](bool success, std::string reason) { result->set_value({success, reason}); });
        cancel();
        cancel();
    });
    ASSERT_EQ(future.wait_for(40s), std::future_status::ready);
    auto [success, reason] = future.get();
    EXPECT_FALSE(success);
    EXPECT_EQ(reason, "echo canceled");
}

TEST(EchoExampleOptionsTest, ValidatesIdentityAndCallbacks) {
    p2pdc_echo::Options options;
    options.capability.fill(std::byte{5});
    options.authorized_peer.fill(std::byte{7});
    Runtime runtime;
    p2pdc_echo::Callbacks callbacks{[](RouteInfo) {}, [] {}, [](std::string) {}};
    EXPECT_TRUE(std::holds_alternative<std::string>(
        p2pdc_echo::EchoPeer::create(options, runtime.executor(), runtime.scheduler(), callbacks)));
    Bytes seed(32, std::byte{1}), secret(32, std::byte{2});
    options.signing_seed = seed;
    options.recipient_secret = secret;
    auto missing = callbacks;
    missing.on_route = {};
    EXPECT_TRUE(std::holds_alternative<std::string>(
        p2pdc_echo::EchoPeer::create(options, runtime.executor(), runtime.scheduler(), missing)));
    auto result = p2pdc_echo::EchoPeer::create(options, runtime.executor(), runtime.scheduler(),
                                               std::move(callbacks));
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<p2pdc_echo::EchoPeer>>(result));
    EXPECT_STREQ(p2pdc_echo::route_name(RouteKind::Direct), "Direct");
    EXPECT_STREQ(p2pdc_echo::route_name(RouteKind::TurnRelayed), "TurnRelayed");
    EXPECT_STREQ(p2pdc_echo::route_name(RouteKind::Unknown), "Unknown");
}
} // namespace
