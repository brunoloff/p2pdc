#include "native_attempt_fixture.hpp"
#include "node_connection_consumer.hpp"
#include "node_race_consumer.hpp"
#include "p2pdc/runtime.h"

namespace {
class NativeSameConsumerTest : public NativeAttemptTest {};

TEST_F(NativeSameConsumerTest, ConnectsAndRejectsInvalidPeers) {
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, completion] {
        (void)p2pdc_test::run_node_connection_consumer(
            config, executor, scheduler, {}, false,
            [completion](bool success) { completion->set_value(success); });
    });
    ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
    EXPECT_TRUE(result.get());
}

class NativeRaceConsumerTest : public NativeAttemptTest,
                               public ::testing::WithParamInterface<p2pdc_test::RaceMode> {};
TEST_P(NativeRaceConsumerTest, ExercisesPublicConnectionRace) {
    Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    DiscoveryConfig config;
    config.network_id = 0x12345683;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    executor([config, executor, scheduler, completion, mode = GetParam()] {
        (void)p2pdc_test::run_node_race_consumer(
            config, executor, scheduler, mode,
            [completion](bool success) { completion->set_value(success); });
    });
    ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
    EXPECT_TRUE(result.get());
}
INSTANTIATE_TEST_SUITE_P(All, NativeRaceConsumerTest,
                         ::testing::Values(p2pdc_test::RaceMode::Collision,
                                           p2pdc_test::RaceMode::Cancel,
                                           p2pdc_test::RaceMode::Reject,
                                           p2pdc_test::RaceMode::CancelAccepted));
} // namespace
