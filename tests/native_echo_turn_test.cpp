#include "echo_example_pair.hpp"
#include "native_attempt_fixture.hpp"
#include "p2pdc/runtime.h"

#include <cstdlib>

namespace {
class NativeEchoTurnExampleTest : public NativeAttemptTest {};

TEST_F(NativeEchoTurnExampleTest, SharedExampleUsesAndReportsTurn) {
    const char *host = std::getenv("P2PDC_TEST_TURN_HOST");
    const char *user = std::getenv("P2PDC_TEST_TURN_USER");
    const char *password = std::getenv("P2PDC_TEST_TURN_PASSWORD");
    ASSERT_TRUE(host && user && password);
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    auto future = result->get_future();
    DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    IceConfig ice;
    ice.servers = {TurnServer{host, 34789, user, password, TurnTransport::Tcp}};
    ice.relay_policy = RelayPolicy::RequireLocalRelay;
    executor([config, executor, scheduler, ice, result] {
        (void)p2pdc_test::run_echo_example_pair(
            config, executor, scheduler, ice, true, false,
            [result](bool success, std::string reason) { result->set_value({success, reason}); });
    });
    ASSERT_EQ(future.wait_for(40s), std::future_status::ready);
    auto [success, reason] = future.get();
    EXPECT_TRUE(success) << reason;
}
} // namespace
