#include "../src/discovery/native_publisher.hpp"
#include "../src/discovery/native_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

TEST(NativePublisherClose, BackendDoesNotSubmitAfterCloseAndReleasesBudget) {
    auto writer = std::make_shared<dht::DhtRunner>();
    auto reader = std::make_shared<dht::DhtRunner>();
    dht::DhtRunner::Config config;
    config.dht_config.node_config.network = 0x12345686;
    config.peer_discovery = false;
    config.peer_publish = false;
    writer->run("127.0.0.1", "::1", "0", config);
    reader->run("127.0.0.1", "::1", "0", config);
    auto connected = std::make_shared<std::promise<bool>>();
    auto ready = connected->get_future();
    reader->bootstrap(writer->getBound(), [connected](bool ok) { connected->set_value(ok); });
    ASSERT_EQ(ready.wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(ready.get());
    std::mutex mutex;
    std::vector<std::function<void()>> tasks;
    std::atomic<unsigned> submitted{0};
    Executor executor = [&](auto task) {
        std::lock_guard lock(mutex);
        tasks.push_back(std::move(task));
        ++submitted;
    };
    const std::array<std::byte, 20> key{std::byte{1}};
    MemoryBudget budget(16);
    NativePublisher publisher(NativeStore(writer, {16, 4, 64}), key, budget, executor);
    unsigned callbacks = 0;
    ASSERT_TRUE(std::holds_alternative<std::monostate>(
        publisher.send(Bytes{std::byte{1}}, [&](bool) { ++callbacks; })));
    publisher.close();
    const auto at_close = submitted.load();
    writer->join();
    reader->join();
    EXPECT_EQ(submitted.load(), at_close);
    for (auto &task : tasks) {
        task();
    }
    EXPECT_EQ(callbacks, 0U);
    EXPECT_EQ(budget.used(), 0U);
}
