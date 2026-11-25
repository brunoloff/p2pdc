#include "../src/discovery/discovery_window.hpp"
#include "../src/transport/native_outgoing_config.hpp"
#include "../src/transport/native_public_connection.hpp"
#include "native_attempt_fixture.hpp"
#include "p2pdc/node.h"

#include <algorithm>
#ifdef P2PDC_TEST_TURN
#include <cstdlib>
#include <stdexcept>
#endif

namespace {
std::uint64_t wall_seconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

#ifdef P2PDC_TEST_TURN
std::string turn_setting(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        throw std::invalid_argument(std::string("missing test setting: ") + name);
    }
    return value;
}

IceConfig public_turn_config(bool valid) {
    IceConfig config;
    config.relay_policy = RelayPolicy::RequireLocalRelay;
    config.servers = {TurnServer{
        turn_setting("P2PDC_TEST_TURN_HOST"), 34789, turn_setting("P2PDC_TEST_TURN_USER"),
        valid ? turn_setting("P2PDC_TEST_TURN_PASSWORD") : "wrong-password", TurnTransport::Tcp}};
    return config;
}
#endif

class NativePublicOutgoingTest : public NativeAttemptTest {
  protected:
    std::unique_ptr<Node> node;
    std::unique_ptr<Lease> lease;
    std::unique_ptr<Subscription> watch;
    std::optional<Advertisement> local_advertisement;
    std::optional<Advertisement> remote_advertisement;
    std::vector<std::function<void()>> timers;
    std::size_t next_timer = 0;
    bool published = false;
    std::vector<Bytes> records;
    bool mailbox_failed = false;
    std::unique_ptr<NativeMailbox> inbox;

    void TearDown() override {
        if (inbox) {
            inbox->close();
        }
        if (node) {
            bool stopped = false;
            if (node->shutdown([&](auto) { stopped = true; })) {
                EXPECT_TRUE(loop.until([&] { return stopped; }));
            }
            node.reset();
        }
        NativeAttemptTest::TearDown();
    }

    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            timers.push_back(std::move(callback));
            return [] {};
        };
    }
    bool advance_until(const std::function<bool()> &condition) {
        return loop.until([&] {
            if (condition()) {
                return true;
            }
            if (next_timer < timers.size()) {
                timers[next_timer++]();
            }
            return false;
        });
    }
    void start_node(const Peer &remote) {
        NativeDiscoveryConfig config;
        config.network_id = 0x12345683;
        config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
        config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
        node = Node::create(config, loop.executor());
        bool started = false;
        ASSERT_TRUE(
            node->start([&](auto result) { started = result.status == NodeStartStatus::Started; }));
        ASSERT_TRUE(loop.until(
            [&] { return started && first->getNodesStats(AF_INET).getKnownNodes() >= 2; }));
        DiscoveryCapability capability{};
        std::copy(remote.capability->begin(), remote.capability->end(), capability.begin());
        const auto remote_key = remote.identity->public_key();
        DiscoveryHandlers handlers{[this, remote_key](Advertisement value, PeerEvent) {
                                       if (value.signing_public_key == remote_key) {
                                           remote_advertisement = std::move(value);
                                       } else {
                                           local_advertisement = std::move(value);
                                       }
                                   },
                                   [](bool) {},
                                   [](WatchError) { ADD_FAILURE() << "discovery failed"; }};
        auto watched = node->watch(capability, scheduler(), {}, std::move(handlers));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(watched));
        watch = std::move(std::get<std::unique_ptr<Subscription>>(watched));
        auto announced =
            node->announce(capability, scheduler(), {},
                           {[this] { published = true; },
                            [](AnnouncementError) { ADD_FAILURE() << "announcement failed"; }});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(announced));
        lease = std::move(std::get<std::unique_ptr<Lease>>(announced));
    }
    bool publish_remote(const Peer &remote) {
        const auto now = wall_seconds();
        AdvertisementFields fields{remote.box->public_key(),
                                   remote.recipient.instance,
                                   remote.address,
                                   1,
                                   0,
                                   now,
                                   now + 50};
        auto encoded =
            encode_advertisement(*remote.identity, fields, *remote.capability, {now, 60, 5});
        auto key = derive_discovery_window_key(*remote.capability, discovery_window_for(now));
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        NativeStore store(first, {1024, 32, 32768});
        if (store.put(std::get<DiscoveryKey>(key), std::get<Bytes>(encoded),
                      [done](bool ok) { done->set_value(ok); })) {
            return false;
        }
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
    std::optional<IncomingOffer> find_offer(Peer &sender, Peer &remote) {
        if (!watch->is_current(*local_advertisement)) {
            ADD_FAILURE() << "local advertisement is no longer current";
            return std::nullopt;
        }
        IncomingOfferResolver resolver({16, 60, 300, 30, 32768, 64});
        std::optional<IncomingOffer> offer;
        std::size_t consumed = 0;
        const bool found = loop.until([&] {
            (void)sender.attempt->poll_start();
            while (consumed < records.size() && !offer) {
                const std::array peers{*local_advertisement};
                auto resolved = resolver.resolve(records[consumed++], *remote.box, remote.recipient,
                                                 *remote.capability, peers, wall_seconds());
                if (auto *value = std::get_if<IncomingOffer>(&resolved)) {
                    offer = std::move(*value);
                }
            }
            return offer.has_value() || sender.failed || mailbox_failed;
        });
        EXPECT_TRUE(found);
        return offer;
    }
    void open_inbox(Peer &remote) {
        inbox = listen(remote, records, mailbox_failed);
        ASSERT_EQ(inbox->registration().wait_for(10s), std::future_status::ready);
        ASSERT_NE(inbox->registration().get(), 0U);
        ASSERT_TRUE(std::holds_alternative<std::monostate>(inbox->catch_up()));
    }
    std::optional<IncomingOffer> find_public_offer(Peer &remote) {
        IncomingOfferResolver resolver({16, 60, 300, 30, 32768, 64});
        std::optional<IncomingOffer> offer;
        std::size_t consumed = 0;
        EXPECT_TRUE(advance_until([&] {
            while (consumed < records.size() && !offer) {
                const std::array peers{*local_advertisement};
                auto resolved = resolver.resolve(records[consumed++], *remote.box, remote.recipient,
                                                 *remote.capability, peers, wall_seconds());
                if (auto *value = std::get_if<IncomingOffer>(&resolved)) {
                    offer = std::move(*value);
                }
            }
            return offer.has_value() || mailbox_failed;
        }));
        return offer;
    }
#ifdef P2PDC_TEST_TURN
    void start_turn_responder(Peer &remote, const IncomingOffer &offer) {
        auto config = responder_config(remote, offer);
        config.now = wall_seconds;
        config.accepted_offer = offer.wire;
        config.ice = public_turn_config(true);
        auto result = NativeAttempt::create(std::move(config), loop.executor(), remote.callbacks());
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeAttempt>>(result));
        remote.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(result));
    }
#endif
    void connect(Peer &sender, Peer &remote, NativeAttemptConfig config) {
        sender.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
            NativeAttempt::create(std::move(config), loop.executor(), sender.callbacks())));
        auto offer = find_offer(sender, remote);
        ASSERT_TRUE(offer);
        ASSERT_FALSE(mailbox_failed);
        auto responder = responder_config(remote, *offer);
        responder.now = wall_seconds;
        responder.accepted_offer = offer->wire;
        remote.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
            NativeAttempt::create(std::move(responder), loop.executor(), remote.callbacks())));
        ASSERT_TRUE(loop.until([&] {
            (void)sender.attempt->poll_start();
            (void)remote.attempt->poll_start();
            return (sender.ready && remote.ready) || sender.failed || remote.failed;
        }));
    }
    void stop_node() {
        bool stopped = false;
        ASSERT_TRUE(node->shutdown(
            [&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
        ASSERT_TRUE(loop.until([&] { return stopped; }));
    }
};
} // namespace

TEST_F(NativePublicOutgoingTest, AdmittedPublicDiscoveryConnectsToAuthorizedResponder) {
    Peer sender(std::byte{1}), remote(std::byte{2});
    start_node(remote);
    ASSERT_TRUE(publish_remote(remote));
    ASSERT_TRUE(
        advance_until([&] { return published && remote_advertisement && local_advertisement; }));
    open_inbox(remote);
    ASSERT_TRUE(inbox);
    auto admission = ConnectionAccess::resolve(*node, *lease, *watch, *remote_advertisement);
    ASSERT_TRUE(std::holds_alternative<ConnectionAdmission>(admission));
    EXPECT_EQ(std::get<ConnectionAdmission>(admission).remote_max_lifetime, 300U);
    EXPECT_EQ(std::get<ConnectionAdmission>(admission).remote_clock_skew, 30U);
    auto config = make_native_outgoing_config(std::get<ConnectionAdmission>(admission), {},
                                              MemoryBudget(4 * 1024 * 1024), wall_seconds);
    connect(sender, remote, std::move(config));
    ASSERT_TRUE(sender.attempt);
    ASSERT_TRUE(remote.attempt);
    ASSERT_EQ(sender.failed, 0);
    ASSERT_EQ(remote.failed, 0);
    ASSERT_EQ(sender.ready, 1);
    ASSERT_EQ(remote.ready, 1);
    ASSERT_EQ(sender.attempt->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !remote.messages.empty(); }));
    EXPECT_EQ(remote.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    ASSERT_EQ(remote.attempt->send(Bytes{std::byte{42}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !sender.messages.empty() && !sender.routes.empty(); }));
    EXPECT_EQ(sender.messages.front(), (Bytes{std::byte{42}}));
    EXPECT_EQ(sender.routes.back().current, RouteKind::Direct);
    sender.attempt->close();
    remote.attempt->close();
    inbox->close();
    stop_node();
}

TEST_F(NativePublicOutgoingTest, PublicConnectionExchangesBytesAndClosesOnNodeShutdown) {
    Peer sender(std::byte{1}), remote(std::byte{2});
    start_node(remote);
    ASSERT_TRUE(publish_remote(remote));
    ASSERT_TRUE(
        advance_until([&] { return published && remote_advertisement && local_advertisement; }));
    open_inbox(remote);
    std::vector<Bytes> messages;
    std::vector<RouteInfo> routes;
    std::vector<ConnectError> errors;
    ConnectionHandlers handlers{[](ConnectionState) {},
                                [&](RouteInfo route) { routes.push_back(route); },
                                [&](Bytes value) { messages.push_back(std::move(value)); }, [] {},
                                [&](ConnectError error) { errors.push_back(error); }};
    auto result =
        node->connect(*lease, *watch, *remote_advertisement, {}, scheduler(), {}, handlers);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    auto connection = std::move(std::get<std::unique_ptr<Connection>>(result));
    auto duplicate =
        node->connect(*lease, *watch, *remote_advertisement, {}, scheduler(), {}, handlers);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(duplicate));
    std::get<std::unique_ptr<Connection>>(duplicate)->close();
    auto offer = find_public_offer(remote);
    ASSERT_TRUE(offer);
    auto config = responder_config(remote, *offer);
    config.now = wall_seconds;
    config.accepted_offer = offer->wire;
    remote.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(config), loop.executor(), remote.callbacks())));
    ASSERT_TRUE(advance_until([&] {
        (void)remote.attempt->poll_start();
        return (connection->state() == ConnectionState::Ready && remote.ready) || !errors.empty();
    }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(connection->send(Bytes{std::byte{0}, std::byte{255}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !remote.messages.empty(); }));
    EXPECT_EQ(remote.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    ASSERT_EQ(remote.attempt->send(Bytes{std::byte{42}}), ChannelSend::Accepted);
    ASSERT_TRUE(advance_until([&] { return !messages.empty() && !routes.empty(); }));
    EXPECT_EQ(messages.front(), (Bytes{std::byte{42}}));
    EXPECT_EQ(routes.back().current, RouteKind::Direct);
    ASSERT_TRUE(advance_until([&] { return connection->round_trip_time().has_value(); }));
    EXPECT_LT(*connection->round_trip_time(), std::chrono::milliseconds{1000});
    EXPECT_EQ(connection->route().round_trip_time, connection->round_trip_time());
    remote.attempt->close();
    inbox->close();
    stop_node();
    EXPECT_EQ(connection->state(), ConnectionState::Closed);
    EXPECT_EQ(connection->send(Bytes{}), SendStatus::Closed);
    EXPECT_EQ(connection->route().current, RouteKind::Unknown);
    EXPECT_FALSE(connection->route().selected_pair);
    EXPECT_FALSE(connection->round_trip_time());
}

TEST_F(NativePublicOutgoingTest, PublicConnectionTimesOutWithoutResponder) {
    Peer remote(std::byte{2});
    start_node(remote);
    ASSERT_TRUE(publish_remote(remote));
    ASSERT_TRUE(advance_until([&] { return published && remote_advertisement; }));
    std::vector<ConnectError> errors;
    ConnectionHandlers handlers{[](ConnectionState) {}, [](RouteInfo) {}, [](Bytes) {}, [] {},
                                [&](ConnectError error) { errors.push_back(error); }};
    ConnectOptions options;
    options.timeout = 50ms;
    options.poll_interval = 10ms;
    auto result =
        node->connect(*lease, *watch, *remote_advertisement, {}, scheduler(), options, handlers);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    auto connection = std::move(std::get<std::unique_ptr<Connection>>(result));
    ASSERT_TRUE(advance_until([&] { return !errors.empty(); }));
    EXPECT_EQ(errors.front(), ConnectError::Timeout);
    EXPECT_EQ(connection->state(), ConnectionState::Failed);
    EXPECT_EQ(connection->send(Bytes{}), SendStatus::Failed);
    connection->close();
    EXPECT_EQ(connection->state(), ConnectionState::Closed);
    auto retry =
        node->connect(*lease, *watch, *remote_advertisement, {}, scheduler(), options, handlers);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(retry));
    std::get<std::unique_ptr<Connection>>(retry)->close();
}

TEST_F(NativePublicOutgoingTest, ConnectionOwnersExchangeBytesUsingApprovedOffer) {
    Peer remote(std::byte{2});
    start_node(remote);
    ASSERT_TRUE(publish_remote(remote));
    ASSERT_TRUE(
        advance_until([&] { return published && remote_advertisement && local_advertisement; }));
    open_inbox(remote);
    std::vector<Bytes> incoming_messages, outgoing_messages;
    std::vector<ConnectError> errors;
    auto handlers = [&](std::vector<Bytes> &messages) {
        return ConnectionHandlers{[](ConnectionState) {}, [](RouteInfo) {},
                                  [&](Bytes value) { messages.push_back(std::move(value)); }, [] {},
                                  [&](ConnectError error) { errors.push_back(error); }};
    };
    auto result = node->connect(*lease, *watch, *remote_advertisement, {}, scheduler(), {},
                                handlers(outgoing_messages));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    auto outgoing = std::move(std::get<std::unique_ptr<Connection>>(result));
    auto offer = find_public_offer(remote);
    ASSERT_TRUE(offer);
    inbox->close();
    auto identity = std::make_shared<EndpointIdentity>();
    identity->signing = remote.identity;
    identity->recipient = remote.box;
    const auto now = wall_seconds();
    AdvertisementFields fields{
        remote.box->public_key(), remote.recipient.instance, remote.address, 1, 0, now, now + 50};
    ConnectionAdmission admission{
        second, {identity, fields}, *local_advertisement, remote.capability, 300, 30};
    auto executor = loop.executor();
    auto incoming = make_public_connection(executor, CallbackGate(executor), scheduler(), {},
                                           handlers(incoming_messages), [] { return true; });
    auto altered = *offer;
    altered.wire.back() ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(incoming->start(admission, {}, &altered)),
                 std::invalid_argument);
    ASSERT_FALSE(incoming->start(admission, {}, &*offer));
    ASSERT_TRUE(advance_until([&] {
        return (incoming->state() == ConnectionState::Ready &&
                outgoing->state() == ConnectionState::Ready) ||
               !errors.empty();
    }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(outgoing->send(Bytes{std::byte{42}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !incoming_messages.empty(); }));
    EXPECT_EQ(incoming_messages.front(), (Bytes{std::byte{42}}));
    ASSERT_EQ(incoming->send(Bytes{std::byte{0}, std::byte{255}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !outgoing_messages.empty(); }));
    EXPECT_EQ(outgoing_messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    incoming->close();
    EXPECT_EQ(incoming->state(), ConnectionState::Closed);
}

#include "native_public_signaling_tests.hpp"

#ifdef P2PDC_TEST_TURN
namespace {
#include "native_outgoing_public_turn_tests.hpp"
} // namespace
#endif
