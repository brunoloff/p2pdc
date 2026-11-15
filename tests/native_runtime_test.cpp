#include "p2pdc/node.h"
#include "p2pdc/runtime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>

using namespace p2pdc;
using namespace std::chrono_literals;

TEST(NativeRuntime, RunsNodeStartupAndShutdown) {
    Runtime runtime;
    NativeDiscoveryConfig config;
    config.network_id = 0x12345695;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    auto node = Node::create(config, runtime.executor());
    std::promise<bool> finished;
    auto result = finished.get_future();
    auto post = runtime.executor();
    post([&] {
        const bool started = node->start([&](NodeStartResult start) {
            EXPECT_EQ(start.status, NodeStartStatus::Started);
            const bool stopping = node->shutdown([&](NodeStopResult stop) {
                EXPECT_EQ(stop.status, NodeStopStatus::Stopped);
                post([&] { finished.set_value(true); });
            });
            if (!stopping) {
                finished.set_value(false);
            }
        });
        if (!started) {
            finished.set_value(false);
        }
    });
    ASSERT_EQ(result.wait_for(20s), std::future_status::ready);
    EXPECT_TRUE(result.get());
}
