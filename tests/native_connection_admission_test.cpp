#include "../src/discovery/connection_admission.hpp"
#include "native_discovery_watch_fixture.hpp"

namespace {
class ConnectionAdmissionTest : public NativeDiscoveryWatchTest {
  protected:
    std::unique_ptr<Node> node;
    std::unique_ptr<Subscription> watch;
    std::unique_ptr<Lease> lease;
    std::optional<Advertisement> remote;
    std::optional<Advertisement> local;
    std::vector<std::function<void()>> timers;
    std::size_t next_timer = 0;
    bool published = false;

    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            timers.push_back(std::move(callback));
            return [] {};
        };
    }
    DiscoveryCapability secret() const {
        DiscoveryCapability value{};
        std::copy(capability->begin(), capability->end(), value.begin());
        return value;
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
    void begin_node() {
        NativeDiscoveryConfig config;
        config.network_id = 0x12345684;
        config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
        config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
        node = Node::create(config, loop.executor());
        bool started = false;
        ASSERT_TRUE(
            node->start([&](auto result) { started = result.status == NodeStartStatus::Started; }));
        ASSERT_TRUE(loop.until([&] { return started; }));
    }
    void begin_watch() {
        const auto expected = identity->public_key();
        DiscoveryHandlers handlers{[this, expected](Advertisement value, PeerEvent) {
                                       if (value.signing_public_key == expected) {
                                           remote = std::move(value);
                                       } else {
                                           local = std::move(value);
                                       }
                                   },
                                   [](bool) {},
                                   [](WatchError) { ADD_FAILURE() << "watch failed"; }};
        auto result = node->watch(secret(), scheduler(), {}, std::move(handlers));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
        watch = std::move(std::get<std::unique_ptr<Subscription>>(result));
        time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
        ASSERT_TRUE(publish_windowed(advertisement(1, time, time + 20), time));
        ASSERT_TRUE(advance_until([&] { return remote.has_value(); }));
    }
    void begin_lease(DiscoveryCapability group) {
        published = false;
        auto result = node->announce(group, scheduler(), {},
                                     {[this] { published = true; },
                                      [](AnnouncementError) { ADD_FAILURE() << "lease failed"; }});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
        lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    }
    void stop_node() {
        bool stopped = false;
        ASSERT_TRUE(node->shutdown(
            [&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
        ASSERT_TRUE(loop.until([&] { return stopped; }));
    }
};
} // namespace

TEST_F(ConnectionAdmissionTest, RequiresPublishedLocalLeaseAndOwnsExactBinding) {
    begin_node();
    begin_watch();
    begin_lease(secret());
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, *remote)),
              AdmissionError::LocalNotPublished);
    ASSERT_TRUE(advance_until([&] { return published && local.has_value(); }));
    auto result = ConnectionAccess::resolve(*node, *lease, *watch, *remote);
    ASSERT_TRUE(std::holds_alternative<ConnectionAdmission>(result));
    auto admission = std::get<ConnectionAdmission>(std::move(result));
    EXPECT_EQ(admission.local.fields, local->fields);
    EXPECT_EQ(admission.local.identity->signing->public_key(), local->signing_public_key);
    EXPECT_EQ(admission.remote, *remote);
    EXPECT_TRUE(
        std::equal(admission.capability->begin(), admission.capability->end(), secret().begin()));
    lease->close();
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, *remote)),
              AdmissionError::Closed);
    EXPECT_TRUE(
        std::holds_alternative<Signature>(admission.local.identity->signing->sign(Bytes{})));
    stop_node();
    node.reset();
    EXPECT_TRUE(
        std::holds_alternative<Signature>(admission.local.identity->signing->sign(Bytes{})));
}

TEST_F(ConnectionAdmissionTest, RejectsAlteredPeerSelfAndClosedWatch) {
    begin_node();
    begin_watch();
    begin_lease(secret());
    ASSERT_TRUE(advance_until([&] { return published && local.has_value(); }));
    auto altered = *remote;
    altered.fields.signaling_inbox[0] ^= std::byte{1};
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, altered)),
              AdmissionError::UnverifiedPeer);
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, *local)),
              AdmissionError::Self);
    watch->close();
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, *remote)),
              AdmissionError::Closed);
    stop_node();
}

TEST_F(ConnectionAdmissionTest, RejectsDifferentGroupAndForeignLease) {
    begin_node();
    begin_watch();
    auto different = secret();
    different[0] ^= std::byte{1};
    begin_lease(different);
    ASSERT_TRUE(advance_until([&] { return published; }));
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, *remote)),
              AdmissionError::DifferentGroup);
    NativeDiscoveryConfig config;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    auto foreign_node = Node::create(config, loop.executor());
    EXPECT_EQ(
        std::get<AdmissionError>(ConnectionAccess::resolve(*foreign_node, *lease, *watch, *remote)),
        AdmissionError::NodeNotReady);
    bool started = false;
    ASSERT_TRUE(foreign_node->start(
        [&](auto result) { started = result.status == NodeStartStatus::Started; }));
    ASSERT_TRUE(loop.until([&] { return started; }));
    EXPECT_EQ(
        std::get<AdmissionError>(ConnectionAccess::resolve(*foreign_node, *lease, *watch, *remote)),
        AdmissionError::ForeignHandle);
    bool stopped = false;
    ASSERT_TRUE(foreign_node->shutdown(
        [&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
    ASSERT_TRUE(loop.until([&] { return stopped; }));
    stop_node();
}

TEST_F(ConnectionAdmissionTest, RejectsDifferentDiscoveryProtocols) {
    begin_node();
    begin_watch();
    AnnounceOptions options;
    options.protocol = DiscoveryProtocol::FixedKeyV1;
    auto result =
        node->announce(secret(), scheduler(), options,
                       {[this] { published = true; }, [](AnnouncementError) { ADD_FAILURE(); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
    lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    ASSERT_TRUE(advance_until([&] { return published; }));
    EXPECT_EQ(std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *watch, *remote)),
              AdmissionError::DifferentGroup);
#ifdef P2PDC_TEST_HAS_NATIVE_CONNECT
    auto listening =
        node->listen(*lease, *watch, scheduler(), {}, {[](Advertisement) {}, [](IncomingError) {}});
    ASSERT_TRUE(std::holds_alternative<IncomingError>(listening));
    EXPECT_EQ(std::get<IncomingError>(listening), IncomingError::InvalidContext);
#endif
    stop_node();
}

TEST_F(ConnectionAdmissionTest, RejectsEitherHandleFromAnotherNode) {
    begin_node();
    begin_watch();
    begin_lease(secret());
    ASSERT_TRUE(advance_until([&] { return published; }));
    NativeDiscoveryConfig config;
    config.network_id = 0x12345684;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    auto other = Node::create(config, loop.executor());
    bool started = false;
    ASSERT_TRUE(
        other->start([&](auto result) { started = result.status == NodeStartStatus::Started; }));
    ASSERT_TRUE(loop.until([&] { return started; }));
    DiscoveryHandlers handlers{[](auto, auto) {}, [](bool) {}, [](auto) {}};
    auto watch_result = other->watch(secret(), scheduler(), {}, std::move(handlers));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(watch_result));
    auto foreign_watch = std::move(std::get<std::unique_ptr<Subscription>>(watch_result));
    auto lease_result = other->announce(secret(), scheduler(), {}, {[] {}, [](auto) {}});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(lease_result));
    auto foreign_lease = std::move(std::get<std::unique_ptr<Lease>>(lease_result));
    EXPECT_EQ(
        std::get<AdmissionError>(ConnectionAccess::resolve(*node, *lease, *foreign_watch, *remote)),
        AdmissionError::ForeignHandle);
    EXPECT_EQ(
        std::get<AdmissionError>(ConnectionAccess::resolve(*node, *foreign_lease, *watch, *remote)),
        AdmissionError::ForeignHandle);
    bool stopped = false;
    ASSERT_TRUE(
        other->shutdown([&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
    ASSERT_TRUE(loop.until([&] { return stopped; }));
    stop_node();
}
