#include "../src/backends/backend_loop.hpp"
#include "../src/backends/client_record_store.hpp"
#include "../src/backends/mqtt_broker.hpp"
#include "../src/backends/nostr_relay.hpp"
#include "../src/backends/socket_channel.hpp"
#include "backend_servers.hpp"
#include "store_sync.hpp"

#include <gtest/gtest.h>

#include <rtc/websocket.hpp>

#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace p2pdc::test;

namespace {
enum class Kind { Nostr, Mqtt };
constexpr StoreLimits limits{1024, 64, 65536};

// Each test gets fresh servers and clients of one kind, on one loop.
class NetworkStoreTest : public ::testing::TestWithParam<Kind> {
  protected:
    std::shared_ptr<RecordClient> client() {
        auto made = GetParam() == Kind::Nostr
                        ? std::shared_ptr<RecordClient>(NostrRelay::create(loop, servers.nostr))
                        : std::shared_ptr<RecordClient>(MqttBroker::create(loop, servers.mqtt));
        clients.push_back(made);
        return made;
    }
    RecordStore store() { return make_client_record_store(loop, client(), limits); }
    void TearDown() override {
        std::promise<void> closed;
        const bool posted = loop->post([&] {
            for (const auto &made : clients)
                made->close();
            closed.set_value();
        });
        if (posted)
            closed.get_future().wait();
        loop->stop();
    }
    BackendServers servers;
    std::shared_ptr<BackendLoop> loop = std::make_shared<BackendLoop>();
    std::vector<std::shared_ptr<RecordClient>> clients;
};

std::string name(const ::testing::TestParamInfo<Kind> &info) {
    return info.param == Kind::Nostr ? "Nostr" : "Mqtt";
}
} // namespace

INSTANTIATE_TEST_SUITE_P(Backends, NetworkStoreTest, ::testing::Values(Kind::Nostr, Kind::Mqtt),
                         name);

TEST_P(NetworkStoreTest, ValuesPutUnderAKeyComeBackFromEveryWriter) {
    auto alice = store(), bob = store();
    const auto one = test_key(1), two = test_key(2);
    EXPECT_EQ(put(alice, one, bytes({1, 2, 3})), true);
    EXPECT_EQ(put(bob, one, bytes({4})), true);
    EXPECT_EQ(put(bob, two, bytes({9})), true);
    const auto found = lookup(alice, one);
    ASSERT_TRUE(found);
    EXPECT_TRUE(found->backend_success);
    EXPECT_FALSE(found->batch.error);
    EXPECT_EQ(sorted(found->batch.values), sorted({bytes({1, 2, 3}), bytes({4})}));
    if (GetParam() == Kind::Nostr) {
        EXPECT_EQ(servers.command("stats")["nostrInvalid"], 0) << "every id and signature held";
    }
    const auto by_content = lookup(bob, one, LookupDeduplication::Content);
    ASSERT_TRUE(by_content);
    EXPECT_EQ(by_content->batch.values.size(), 2U);
}

TEST_P(NetworkStoreTest, AWatchHearsOfValuesPutAfterItIsReady) {
    auto watcher = store(), writer = store();
    Heard heard;
    auto result = watcher.watch(test_key(3), heard.callback(), false);
    ASSERT_TRUE(std::holds_alternative<RecordWatch>(result));
    auto &watch = std::get<RecordWatch>(result);
    ASSERT_EQ(registered(watch), 1U);
    EXPECT_EQ(put(writer, test_key(3), bytes({7})), true);
    EXPECT_EQ(put(writer, test_key(4), bytes({8})), true);
    ASSERT_TRUE(heard.wait_values(1));
    watch.cancel();
    EXPECT_EQ(put(writer, test_key(3), bytes({5})), true);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(heard.values(), std::vector<Bytes>{bytes({7})});
}

TEST_P(NetworkStoreTest, RefusesInvalidRequestsAtOnce) {
    auto first = store();
    EXPECT_EQ(first.put(Bytes(20), bytes({1}), [](bool) {}), StoreError::InvalidKey);
    EXPECT_EQ(first.put(test_key(1), Bytes(2000), [](bool) {}), StoreError::ResourceLimit);
    EXPECT_EQ(first.put(test_key(1), bytes({1}), {}), StoreError::InvalidCallback);
    EXPECT_EQ(first.lookup(
                  Bytes(19), [](LookupResult) {}, LookupDeduplication::Content),
              StoreError::InvalidKey);
    auto watch = first.watch(test_key(1), {}, false);
    EXPECT_EQ(std::get<StoreError>(watch), StoreError::InvalidCallback);
}

TEST_P(NetworkStoreTest, RefusalsSayWhetherTryingAgainMayHelp) {
    auto first = store();
    const bool nostr = GetParam() == Kind::Nostr;
    servers.command(nostr ? "nostr-refuse blocked: not here" : "mqtt-refuse-publish 135");
    EXPECT_EQ(put(first, test_key(5), bytes({1})), false);
    servers.command(nostr ? "nostr-close-next blocked: no" : "mqtt-refuse-subscribe 135");
    Heard heard;
    auto result = first.watch(test_key(5), heard.callback(), false);
    auto &watch = std::get<RecordWatch>(result);
    EXPECT_EQ(registered(watch), 0U);
    ASSERT_TRUE(heard.wait_error());
    EXPECT_EQ(heard.errors().front(), StoreError::PermanentFailure);
    EXPECT_EQ(put(first, test_key(5), bytes({2})), true);
}

TEST_P(NetworkStoreTest, LosingTheServerEndsWatchesAndTheStoreReconnects) {
    auto first = store();
    Heard heard;
    auto result = first.watch(test_key(6), heard.callback(), false);
    ASSERT_EQ(registered(std::get<RecordWatch>(result)), 1U);
    servers.command("drop-all");
    ASSERT_TRUE(heard.wait_error());
    EXPECT_EQ(heard.errors().front(), StoreError::NotRunning);
    // The channel waits a second before opening again.
    std::optional<bool> stored;
    for (int attempt = 0; attempt < 30 && stored != true; ++attempt) {
        stored = put(first, test_key(6), bytes({4}));
        if (stored != true)
            std::this_thread::sleep_for(200ms);
    }
    EXPECT_EQ(stored, true);
    const auto found = lookup(first, test_key(6));
    ASSERT_TRUE(found);
    EXPECT_EQ(found->batch.values, std::vector<Bytes>{bytes({4})});
}

TEST_P(NetworkStoreTest, StoresOfOneClientShareItsConnection) {
    auto shared = client();
    auto small = make_client_record_store(loop, shared, {16, 8, 128});
    auto large = make_client_record_store(loop, shared, limits);
    EXPECT_EQ(put(large, test_key(7), Bytes(100, std::byte{1})), true);
    EXPECT_EQ(put(large, test_key(7), bytes({2})), true);
    const auto found = lookup(small, test_key(7));
    ASSERT_TRUE(found);
    EXPECT_EQ(found->batch.values, std::vector<Bytes>{bytes({2})}) << "the small store's limits";
    if (GetParam() == Kind::Mqtt) {
        EXPECT_EQ(servers.command("stats")["mqttConnects"], 1);
    }
}

TEST_P(NetworkStoreTest, OperationsFailOnceTheLoopHasStopped) {
    auto first = store();
    TearDown();
    clients.clear();
    EXPECT_EQ(first.put(test_key(1), bytes({1}), [](bool) {}), StoreError::NotRunning);
    EXPECT_EQ(first.lookup(
                  test_key(1), [](LookupResult) {}, LookupDeduplication::Content),
              StoreError::NotRunning);
    EXPECT_EQ(std::get<StoreError>(first.watch(
                  test_key(1), [](StoreBatch) {}, false)),
              StoreError::NotRunning);
}

// Wire compatibility: the browser's own clients (src/browser/, run by node) and native ones
// read each other's values and hear each other's puts through the same server.
TEST_P(NetworkStoreTest, BrowserAndNativeClientsReadAndHearEachOther) {
    const std::string kind = GetParam() == Kind::Nostr ? "nostr" : "mqtt";
    const auto key = test_key(8);
    const auto hex = to_hex(std::span<const std::byte>(key));
    auto native = store();
    ASSERT_EQ(servers.command("js-put " + kind + " " + hex + " 0a0b")["ok"], true);
    EXPECT_EQ(put(native, key, bytes({0x0c})), true);
    const auto found = lookup(native, key);
    ASSERT_TRUE(found);
    EXPECT_EQ(sorted(found->batch.values), sorted({bytes({0x0a, 0x0b}), bytes({0x0c})}));
    EXPECT_EQ(servers.command("js-get " + kind + " " + hex)["values"],
              nlohmann::json::array({"0a0b", "0c"}));

    Heard heard;
    auto result = native.watch(key, heard.callback(), false);
    ASSERT_EQ(registered(std::get<RecordWatch>(result)), 1U);
    ASSERT_EQ(servers.command("js-listen " + kind + " " + hex)["ready"], true);
    ASSERT_EQ(servers.command("js-put " + kind + " " + hex + " 0d")["ok"], true);
    EXPECT_EQ(put(native, key, bytes({0x0e})), true);
    // Nostr relays replay events from the second a watch began (NIP-01 `since` is inclusive),
    // so a watch may also hear the values put just before it: both new ones must arrive.
    const auto has = [](const auto &values, const auto &value) {
        return std::ranges::find(values, value) != values.end();
    };
    for (int wait = 0; wait < 100; ++wait) {
        const auto values = heard.values();
        if (has(values, bytes({0x0d})) && has(values, bytes({0x0e})))
            break;
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(has(heard.values(), bytes({0x0d})));
    EXPECT_TRUE(has(heard.values(), bytes({0x0e})));
    nlohmann::json js_heard;
    for (int wait = 0; wait < 100; ++wait) {
        js_heard = servers.command("js-heard " + kind + " " + hex)["values"];
        if (has(js_heard, "0d") && has(js_heard, "0e"))
            break;
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(has(js_heard, "0d") && has(js_heard, "0e")) << js_heard;
    if (GetParam() == Kind::Mqtt) { // brokers send live messages only to watches
        EXPECT_EQ(heard.values().size(), 2U);
        EXPECT_EQ(js_heard.size(), 2U);
    }
}

// Certificates are checked: a server that no authority vouches for is refused, though it
// answers once checks are turned off, so the refusal comes from the check.
TEST(SocketChannelTls, RefusesAServerWhoseCertificateIsNotTrusted) {
    BackendServers servers;
    auto loop = std::make_shared<BackendLoop>();
    auto channel =
        SocketChannel::create(loop, servers.untrusted, {}, {[](SocketMessage) {}, [] {}});
    std::promise<std::optional<BackendFailure>> refused;
    loop->post([&] { channel->ready([&](auto failure) { refused.set_value(failure); }); });
    auto result = refused.get_future();
    ASSERT_EQ(result.wait_for(15s), std::future_status::ready);
    EXPECT_EQ(result.get(), BackendFailure::Transient);
    loop->post([channel] { channel->close(); });
    loop->stop();

    rtc::WebSocket::Configuration unchecked;
    unchecked.disableTlsVerification = true;
    rtc::WebSocket socket(unchecked);
    std::promise<std::string> echoed;
    socket.onOpen([&] { socket.send(std::string("hello")); });
    socket.onMessage([&](rtc::message_variant data) {
        if (auto *text = std::get_if<std::string>(&data))
            echoed.set_value(*text);
    });
    socket.open(servers.untrusted);
    auto echo = echoed.get_future();
    ASSERT_EQ(echo.wait_for(15s), std::future_status::ready);
    EXPECT_EQ(echo.get(), "hello");
    socket.resetCallbacks();
    socket.close();
}
