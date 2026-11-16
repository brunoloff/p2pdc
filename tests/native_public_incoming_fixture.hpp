#pragma once

#include "../src/discovery/connection_admission.hpp"
#include "../src/discovery/discovery_window.hpp"
#include "native_attempt_fixture.hpp"
#include "p2pdc/node.h"

#include <algorithm>
#include <stdexcept>
#ifdef P2PDC_TEST_TURN
#include <cstdlib>
#endif

namespace {
#ifdef P2PDC_TEST_TURN
std::string public_turn_setting(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        throw std::invalid_argument(std::string("missing test setting: ") + name);
    return value;
}
IceConfig public_turn(RelayPolicy policy, bool valid = true) {
    return {{TurnServer{public_turn_setting("P2PDC_TEST_TURN_HOST"), 34789,
                        public_turn_setting("P2PDC_TEST_TURN_USER"),
                        valid ? public_turn_setting("P2PDC_TEST_TURN_PASSWORD") : "wrong-password",
                        TurnTransport::Tcp}},
            policy};
}
#endif
class NativePublicIncomingTest : public NativeAttemptTest {
  protected:
    struct Side {
        std::unique_ptr<Node> node;
        std::unique_ptr<Subscription> watch;
        std::unique_ptr<Lease> lease;
        std::vector<Advertisement> observed;
        std::optional<Advertisement> peer;
        bool published = false;
    };
    Side sender, receiver;
    std::vector<std::function<void()>> timers;
    std::size_t next_timer = 0;
    std::vector<IncomingError> incoming_errors;
    std::vector<ConnectError> connection_errors;
    std::unique_ptr<IncomingListener> incoming;
    std::optional<Advertisement> request;
    std::unique_ptr<Connection> sending, receiving;
    std::vector<Bytes> sent, received;
    std::size_t max_connections = 8;

    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            timers.push_back(std::move(callback));
            return [] {};
        };
    }
    bool advance(const std::function<bool()> &condition) {
        return loop.until([&] {
            if (condition())
                return true;
            if (next_timer < timers.size())
                timers[next_timer++]();
            return false;
        });
    }
    void start_side(Side &side) {
        NativeDiscoveryConfig config;
        config.network_id = 0x12345683;
        config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
        config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
        config.max_connections = max_connections;
        side.node = Node::create(config, loop.executor());
        bool started = false;
        ASSERT_TRUE(side.node->start(
            [&](auto value) { started = value.status == NodeStartStatus::Started; }));
        ASSERT_TRUE(loop.until([&] { return started; }));
        DiscoveryCapability capability{};
        std::fill(capability.begin(), capability.end(), std::byte{8});
        WatchOptions watch_options;
        watch_options.clock_skew = 0;
        auto watched = side.node->watch(
            capability, scheduler(), watch_options,
            {[&](Advertisement value, PeerEvent) { side.observed.push_back(std::move(value)); },
             [](bool) {}, [](WatchError) { ADD_FAILURE() << "watch failed"; }});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(watched));
        side.watch = std::move(std::get<std::unique_ptr<Subscription>>(watched));
        auto announced =
            side.node->announce(capability, scheduler(), {},
                                {[&] { side.published = true; },
                                 [](AnnouncementError) { ADD_FAILURE() << "lease failed"; }});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(announced));
        side.lease = std::move(std::get<std::unique_ptr<Lease>>(announced));
    }
    bool find_peer(Side &side) {
        for (const auto &peer : side.observed) {
            if (std::holds_alternative<ConnectionAdmission>(
                    ConnectionAccess::resolve(*side.node, *side.lease, *side.watch, peer))) {
                side.peer = peer;
                return true;
            }
        }
        return false;
    }
    void setup_sides() {
        start_side(sender);
        start_side(receiver);
        ASSERT_TRUE(advance([&] {
            return sender.published && receiver.published && find_peer(sender) &&
                   find_peer(receiver);
        }));
    }
    ConnectionHandlers handlers(std::vector<Bytes> &messages) {
        return {[](ConnectionState) {}, [](RouteInfo) {},
                [&](Bytes bytes) { messages.push_back(std::move(bytes)); }, [] {},
                [&](ConnectError error) { connection_errors.push_back(error); }};
    }
    void listen(IncomingOptions options = {}) {
        auto result =
            receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), options,
                                  {[&](Advertisement peer) { request = std::move(peer); },
                                   [&](IncomingError error) { incoming_errors.push_back(error); }});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(result));
        incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(result));
    }
    void connect_sender(IceConfig ice = {}, ConnectOptions options = {}) {
        auto result = sender.node->connect(*sender.lease, *sender.watch, *sender.peer,
                                           std::move(ice), scheduler(), options, handlers(sent));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
        sending = std::move(std::get<std::unique_ptr<Connection>>(result));
    }
#ifdef P2PDC_TEST_TURN
    void verify_relay_exchange_and_close() {
        ASSERT_EQ(sending->send(Bytes{std::byte{42}}), SendStatus::Accepted);
        ASSERT_TRUE(loop.until([&] { return !received.empty(); }));
        EXPECT_EQ(received.front(), (Bytes{std::byte{42}}));
        ASSERT_EQ(receiving->send(Bytes{std::byte{0}, std::byte{255}}), SendStatus::Accepted);
        ASSERT_TRUE(loop.until([&] { return !sent.empty(); }));
        EXPECT_EQ(sent.front(), (Bytes{std::byte{0}, std::byte{255}}));
        sending->close();
        receiving->close();
        EXPECT_EQ(sending->route().current, RouteKind::Unknown);
        EXPECT_EQ(receiving->route().current, RouteKind::Unknown);
        EXPECT_FALSE(sending->route().selected_pair);
        EXPECT_FALSE(receiving->route().selected_pair);
        EXPECT_EQ(sending->route().turn_history, TurnHistory::Used);
        EXPECT_EQ(receiving->route().turn_history, TurnHistory::Used);
    }
#endif
    bool put(std::array<std::byte, 20> key, Bytes value) {
        auto completion = std::make_shared<std::promise<bool>>();
        auto future = completion->get_future();
        NativeStore store(first, {32768, 128, 1048576});
        if (store.put(key, std::move(value), [completion](bool ok) { completion->set_value(ok); }))
            return false;
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
    static std::uint64_t wall_seconds() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }
    void publish_external(Peer &external) {
        const auto now = wall_seconds();
        AdvertisementFields fields{external.box->public_key(),
                                   external.recipient.instance,
                                   external.address,
                                   1,
                                   0,
                                   now,
                                   now + 30};
        auto advertisement =
            encode_advertisement(*external.identity, fields, *external.capability, {now, 60, 0});
        ASSERT_TRUE(std::holds_alternative<Bytes>(advertisement));
        auto key = derive_discovery_window_key(*external.capability, discovery_window_for(now));
        ASSERT_TRUE(put(std::get<DiscoveryKey>(key), std::get<Bytes>(advertisement)));
        ASSERT_TRUE(advance([&] {
            return std::any_of(
                receiver.observed.begin(), receiver.observed.end(), [&](const auto &peer) {
                    return peer.signing_public_key == external.identity->public_key();
                });
        }));
    }
    void publish_short_offer(Peer &external, std::uint64_t &expiry) {
        const auto issued = wall_seconds();
        const auto &target = *sender.peer;
        SignalingFields offer{SignalingKind::Offer,
                              external.recipient.instance,
                              {target.signing_public_key, target.fields.instance_id},
                              AttemptId{std::byte{12}},
                              0,
                              issued,
                              issued + 2,
                              Bytes{std::byte{1}}};
        auto wire = encode_signaling(*external.identity, offer, target.fields.recipient_public_key,
                                     *external.capability, {issued, 60, 0, 32768});
        ASSERT_TRUE(std::holds_alternative<Bytes>(wire));
        ASSERT_TRUE(put(target.fields.signaling_inbox, std::get<Bytes>(wire)));
        expiry = offer.expires_at;
    }
    void publish_cancel(Peer &external, AttemptId attempt) {
        const auto now = wall_seconds();
        const auto &target = *sender.peer;
        SignalingFields fields{SignalingKind::Cancel,
                               external.recipient.instance,
                               {target.signing_public_key, target.fields.instance_id},
                               attempt,
                               63,
                               now,
                               now + 30,
                               {}};
        auto wire = encode_signaling(*external.identity, fields, target.fields.recipient_public_key,
                                     *external.capability, {now, 60, 0, 32768});
        ASSERT_TRUE(std::holds_alternative<Bytes>(wire));
        ASSERT_TRUE(put(target.fields.signaling_inbox, std::get<Bytes>(wire)));
    }
    void TearDown() override {
        for (auto *side : {&sender, &receiver}) {
            if (side->node) {
                bool stopped = false;
                if (side->node->shutdown([&](auto) { stopped = true; })) {
                    EXPECT_TRUE(loop.until([&] { return stopped; }));
                }
                side->node.reset();
            }
        }
        NativeAttemptTest::TearDown();
    }
};
} // namespace
