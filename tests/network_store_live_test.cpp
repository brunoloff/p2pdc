// Opt-in tests against public servers, like tests/discovery_backends_live_test.mjs: set
// P2PDC_TEST_NOSTR_RELAYS and P2PDC_TEST_MQTT_BROKERS to space-separated wss:// URLs, for
// example "wss://nos.lol wss://relay.primal.net" and "wss://broker.hivemq.com:8884/mqtt".
#include "../src/backends/backend_loop.hpp"
#include "../src/backends/client_record_store.hpp"
#include "../src/backends/mqtt_broker.hpp"
#include "../src/backends/nostr_relay.hpp"
#include "store_sync.hpp"

#include <gtest/gtest.h>
#include <sodium.h>

#include <cstdlib>
#include <sstream>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace p2pdc::test;

namespace {
std::vector<std::string> servers(const char *name) {
    std::vector<std::string> found;
    std::stringstream words(std::getenv(name) ? std::getenv(name) : "");
    for (std::string word; words >> word;)
        found.push_back(word);
    return found;
}

// Two clients of one server: one writes, the other finds the value and hears the next.
void exchange(
    const std::function<std::shared_ptr<RecordClient>(std::shared_ptr<BackendLoop>)> &make) {
    auto loop = std::make_shared<BackendLoop>();
    auto writer_client = make(loop), watcher_client = make(loop);
    const StoreLimits limits{1024, 64, 65536};
    auto writer = make_client_record_store(loop, writer_client, limits);
    auto watcher = make_client_record_store(loop, watcher_client, limits);
    std::array<std::byte, 20> key{};
    randombytes_buf(key.data(), key.size()); // a key no one else uses
    EXPECT_EQ(put(writer, key, bytes({1, 2, 3})), true);
    const auto found = lookup(watcher, key);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->batch.values, std::vector<Bytes>{bytes({1, 2, 3})});
    Heard heard;
    auto result = watcher.watch(key, heard.callback(), false);
    ASSERT_EQ(registered(std::get<RecordWatch>(result)), 1U);
    EXPECT_EQ(put(writer, key, bytes({4})), true);
    EXPECT_TRUE(heard.wait_values(1, 20s));
    std::get<RecordWatch>(result).cancel();
    std::promise<void> closed;
    loop->post([&] {
        writer_client->close();
        watcher_client->close();
        closed.set_value();
    });
    closed.get_future().wait();
    loop->stop();
}
} // namespace

TEST(NetworkStoreLive, NostrRelays) {
    const auto urls = servers("P2PDC_TEST_NOSTR_RELAYS");
    if (urls.empty())
        GTEST_SKIP() << "set P2PDC_TEST_NOSTR_RELAYS";
    for (const auto &url : urls) {
        SCOPED_TRACE(url);
        exchange([&](auto loop) { return NostrRelay::create(loop, url); });
    }
}

TEST(NetworkStoreLive, MqttBrokers) {
    const auto urls = servers("P2PDC_TEST_MQTT_BROKERS");
    if (urls.empty())
        GTEST_SKIP() << "set P2PDC_TEST_MQTT_BROKERS";
    for (const auto &url : urls) {
        SCOPED_TRACE(url);
        exchange([&](auto loop) { return MqttBroker::create(loop, url); });
    }
}
