// The native PeerJS backend against the strict test server (tests/support/peerjs_server.mjs),
// over real libdatachannel data connections on this computer, as peerjs_switchboard_test.mjs
// tests the JavaScript one.
#include "../src/backends/backend_loop.hpp"
#include "../src/backends/client_record_store.hpp"
#include "../src/backends/peerjs_switchboard.hpp"
#include "backend_servers.hpp"
#include "store_sync.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace p2pdc::test;

namespace {
constexpr StoreLimits limits{1024, 64, 65536};

class PeerJsStoreTest : public ::testing::Test {
  protected:
    // Host candidates are enough on one computer; no STUN or TURN servers.
    std::shared_ptr<PeerJsSwitchboard> board(const std::string &key = "peerjs",
                                             const std::string *url = nullptr) {
        auto made =
            PeerJsSwitchboard::create(loop, url ? *url : servers.peerjs, key, rtc::Configuration{});
        boards.push_back(made);
        return made;
    }
    RecordStore store(const std::shared_ptr<PeerJsSwitchboard> &client) {
        return make_client_record_store(loop, client, limits);
    }
    void close(const std::shared_ptr<PeerJsSwitchboard> &client) {
        std::promise<void> closed;
        loop->post([&] {
            client->close();
            closed.set_value();
        });
        closed.get_future().wait();
    }
    void TearDown() override {
        for (const auto &made : boards)
            close(made);
        loop->stop();
    }
    BackendServers servers;
    std::shared_ptr<BackendLoop> loop = std::make_shared<BackendLoop>();
    std::vector<std::shared_ptr<PeerJsSwitchboard>> boards;
};

std::string host_id(int seed) {
    const auto key = test_key(seed);
    return "p2pdc" + to_hex(std::span<const std::byte>(key));
}
} // namespace

TEST_F(PeerJsStoreTest, TheFirstStoreHostsAKeyAndOthersReachItOverADataConnection) {
    auto alice = store(board()), bob = store(board());
    EXPECT_EQ(put(alice, test_key(1), bytes({1, 2})), true);
    EXPECT_EQ(put(bob, test_key(1), bytes({3})), true);
    for (const auto *reader : {&bob, &alice}) {
        const auto found = lookup(*reader, test_key(1));
        ASSERT_TRUE(found);
        EXPECT_FALSE(found->batch.error);
        EXPECT_EQ(sorted(found->batch.values), sorted({bytes({1, 2}), bytes({3})}));
    }
    const auto stats = servers.command("stats");
    const auto clients = stats["peerjsClients"].get<std::vector<std::string>>();
    EXPECT_TRUE(std::ranges::find(clients, host_id(1)) != clients.end()) << "alice hosts the key";
    EXPECT_EQ(stats["peerjsDropped"], 0)
        << "only genuine PeerJS signalling went through the server";
    EXPECT_GT(stats["peerjsMessages"], 0);
}

TEST_F(PeerJsStoreTest, WatchesHearNewValuesOnTheHostAndElsewhere) {
    auto host = store(board()), watcher = store(board()), writer = store(board());
    EXPECT_EQ(put(host, test_key(3), bytes({1})), true); // host claims the key
    Heard local, remote;
    auto local_watch = host.watch(test_key(3), local.callback(), false);
    auto remote_watch = watcher.watch(test_key(3), remote.callback(), false);
    ASSERT_EQ(registered(std::get<RecordWatch>(local_watch)), 1U);
    ASSERT_EQ(registered(std::get<RecordWatch>(remote_watch)), 1U);
    EXPECT_EQ(put(writer, test_key(3), bytes({2})), true);
    ASSERT_TRUE(local.wait_values(1));
    ASSERT_TRUE(remote.wait_values(1));
    EXPECT_EQ(local.values(), std::vector<Bytes>{bytes({2})});
    EXPECT_EQ(remote.values(), std::vector<Bytes>{bytes({2})});
}

TEST_F(PeerJsStoreTest, WhenTheHostLeavesTheNextStoreTakesOverAndWritersPutAgain) {
    auto first_board = board();
    auto first = store(first_board), second = store(board()), third = store(board());
    EXPECT_EQ(put(first, test_key(4), bytes({1})), true);
    EXPECT_EQ(put(second, test_key(4), bytes({2})), true);
    close(first_board);
    std::optional<LookupResult> found;
    for (int attempt = 0; attempt < 50; ++attempt) {
        found = lookup(third, test_key(4));
        if (found && found->batch.values == std::vector<Bytes>{bytes({2})})
            break;
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(found);
    EXPECT_EQ(found->batch.values, std::vector<Bytes>{bytes({2})})
        << "second put its value again on the new host";
    EXPECT_EQ(put(third, test_key(4), bytes({3})), true);
    const auto after = lookup(second, test_key(4));
    ASSERT_TRUE(after);
    EXPECT_EQ(sorted(after->batch.values), sorted({bytes({2}), bytes({3})}));
}

TEST_F(PeerJsStoreTest, AWrongServerKeyIsRefusedForGood) {
    auto refused = store(board("wrong"));
    std::optional<bool> stored = put(refused, test_key(5), bytes({1}));
    EXPECT_EQ(stored, false);
    Heard heard;
    auto watch = refused.watch(test_key(5), heard.callback(), false);
    EXPECT_EQ(registered(std::get<RecordWatch>(watch)), 0U);
    ASSERT_TRUE(heard.wait_error());
    EXPECT_EQ(heard.errors().front(), StoreError::PermanentFailure);
}

TEST_F(PeerJsStoreTest, AConnectionClosedForATakenIdLeadsToTheKeysHost) {
    auto host = store(board("peerjs", &servers.silent_peerjs));
    auto other = store(board("peerjs", &servers.silent_peerjs));
    EXPECT_EQ(put(host, test_key(6), bytes({1})), true);
    const auto found = lookup(other, test_key(6));
    ASSERT_TRUE(found);
    EXPECT_FALSE(found->batch.error);
    EXPECT_EQ(found->batch.values, std::vector<Bytes>{bytes({1})});
}

TEST(PeerJsSocketUrl, ReplacesTheQueryWithKeyIdAndToken) {
    EXPECT_EQ(peerjs_socket_url("wss://0.peerjs.com/peerjs?key=old", "peer js", "p2pdcab", "t1"),
              "wss://0.peerjs.com/peerjs?key=peer%20js&id=p2pdcab&token=t1");
}

// A browser and a native node share PeerJS keys: each hosts a key for the other, over a real
// data connection between Chromium's WebRTC and libdatachannel. Opt-in, as it starts Chromium:
// set P2PDC_TEST_CHROMIUM to its executable.
TEST_F(PeerJsStoreTest, BrowserAndNativeNodesHostKeysForEachOther) {
    if (!std::getenv("P2PDC_TEST_CHROMIUM"))
        GTEST_SKIP() << "set P2PDC_TEST_CHROMIUM";
    ASSERT_EQ(servers.command("browser-open")["ok"], true);
    auto native = store(board());
    const auto hex = [](int seed) {
        const auto key = test_key(seed);
        return to_hex(std::span<const std::byte>(key));
    };
    const auto wait_heard = [&](const std::string &key, std::size_t count) {
        nlohmann::json heard;
        for (int attempt = 0; attempt < 100 && heard.size() < count; ++attempt) {
            heard = servers.command("browser-heard " + key)["values"];
            std::this_thread::sleep_for(50ms);
        }
        return heard;
    };

    // The browser hosts key 20; the native node reaches it.
    const auto put_first = servers.command("browser-put " + hex(20) + " 0a");
    ASSERT_EQ(put_first["hosts"], true) << put_first;
    Heard heard;
    auto watch = native.watch(test_key(20), heard.callback(), false);
    ASSERT_EQ(registered(std::get<RecordWatch>(watch), 20s), 1U);
    EXPECT_EQ(put(native, test_key(20), bytes({0x0b}), 20s), true);
    const auto found = lookup(native, test_key(20));
    ASSERT_TRUE(found);
    EXPECT_EQ(sorted(found->batch.values), sorted({bytes({0x0a}), bytes({0x0b})}));
    ASSERT_EQ(servers.command("browser-put " + hex(20) + " 0c")["ok"], true);
    ASSERT_TRUE(heard.wait_values(2));
    const auto values = heard.values();
    EXPECT_TRUE(std::ranges::find(values, bytes({0x0c})) != values.end());

    // The native node hosts key 21; the browser reaches it.
    EXPECT_EQ(put(native, test_key(21), bytes({0x01}), 20s), true);
    ASSERT_EQ(servers.command("browser-listen " + hex(21))["ready"], true);
    const auto browser_put = servers.command("browser-put " + hex(21) + " 02");
    EXPECT_EQ(browser_put["hosts"], false) << browser_put;
    EXPECT_EQ(servers.command("browser-get " + hex(21))["values"],
              nlohmann::json::array({"01", "02"}));
    EXPECT_EQ(put(native, test_key(21), bytes({0x03})), true);
    EXPECT_EQ(wait_heard(hex(21), 2), nlohmann::json::array({"02", "03"}));
    EXPECT_EQ(servers.command("stats")["peerjsDropped"], 0);
}
