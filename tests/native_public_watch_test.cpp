#include "../src/discovery/native_public_watch.hpp"
#include "native_discovery_watch_fixture.hpp"
#include "node_discovery_consumer.hpp"
#include "p2pdc/node.h"

#include <algorithm>

namespace {
class PublicWatchTest : public NativeDiscoveryWatchTest {
  protected:
    bool publish(ByteView wire) { return publish_windowed(wire, time); }
    std::vector<std::function<void()>> timers;
    std::size_t next_timer = 0;
    std::unique_ptr<Node> node;
    std::unique_ptr<Node> peer_node;
    std::unique_ptr<Subscription> subscription;
    std::vector<PeerEvent> peers;
    std::vector<WatchError> failures;
    std::optional<bool> caught;

    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            timers.push_back(std::move(callback));
            return [] {};
        };
    }
    DiscoveryHandlers handlers() {
        return {[this](Advertisement, PeerEvent event) { peers.push_back(event); },
                [this](bool success) { caught = success; },
                [this](WatchError error) { failures.push_back(error); }};
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
    void start_node() {
        NativeDiscoveryConfig options;
        options.network_id = 0x12345684;
        options.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
        options.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
        node = Node::create(options, loop.executor());
        bool started = false;
        ASSERT_TRUE(
            node->start([&](auto result) { started = result.status == NodeStartStatus::Started; }));
        ASSERT_TRUE(loop.until([&] { return started; }));
    }
    DiscoveryCapability secret() const {
        DiscoveryCapability value{};
        std::copy(capability->begin(), capability->end(), value.begin());
        return value;
    }
    void start_watch(DiscoveryHandlers callbacks) {
        auto result = node->watch(secret(), scheduler(), {}, std::move(callbacks));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
        subscription = std::move(std::get<std::unique_ptr<Subscription>>(result));
        ASSERT_TRUE(subscription);
    }
    void stop_node() {
        bool stopped = false;
        ASSERT_TRUE(node->shutdown(
            [&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
        ASSERT_TRUE(loop.until([&] { return stopped; }));
    }
    void start_peer() {
        NativeDiscoveryConfig options;
        options.network_id = 0x12345684;
        options.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
        options.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
        peer_node = Node::create(options, loop.executor());
        bool started = false;
        ASSERT_TRUE(peer_node->start(
            [&](auto result) { started = result.status == NodeStartStatus::Started; }));
        ASSERT_TRUE(loop.until([&] { return started; }));
    }
    void stop_peer() {
        bool stopped = false;
        ASSERT_TRUE(peer_node->shutdown(
            [&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
        ASSERT_TRUE(loop.until([&] { return stopped; }));
    }
};
} // namespace

TEST_F(PublicWatchTest, SharedConsumerDiscoversAndShutsDown) {
    DiscoveryConfig config;
    config.network_id = 0x12345684;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    bool completed = false, success = false;
    auto cancel = p2pdc_test::run_node_discovery_consumer(
        config, loop.executor(), scheduler(), NativeStore(first, {32768, 128, 1048576}),
        [&](bool value) {
            success = value;
            completed = true;
        });
    const bool finished = advance_until([&] { return completed; });
    cancel();
    ASSERT_TRUE(finished);
    EXPECT_TRUE(success);
}

TEST_F(PublicWatchTest, DiscoversVerifiedPeerAndStopsWithNode) {
    start_node();
    start_watch(handlers());
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    ASSERT_TRUE(advance_until([&] { return !peers.empty(); }));
    EXPECT_EQ(peers.front(), PeerEvent::Seen);
    EXPECT_TRUE(failures.empty());
    EXPECT_TRUE(subscription->healthy());
    stop_node();
    EXPECT_FALSE(subscription->healthy());
}

TEST_F(PublicWatchTest, InternalSnapshotCarriesVerifiedPeersAndPolicy) {
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    WatchOptions options;
    options.max_advertisement_lifetime = 60;
    options.clock_skew = 5;
    auto result =
        make_public_watch(NativeStore(second, {1024, 512, 524288}), secret(), scheduler(), options,
                          handlers(), loop.executor(),
                          CallbackGate(loop.executor()), MemoryBudget(4 * 1024 * 1024));
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<detail::DiscoveryBackend>>(result));
    auto watch = std::get<std::shared_ptr<detail::DiscoveryBackend>>(std::move(result));
    auto empty = watch->current_peers();
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->peers.empty());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    ASSERT_TRUE(advance_until([&] { return !peers.empty(); }));
    auto snapshot = watch->current_peers();
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->peers.size(), 1U);
    EXPECT_EQ(snapshot->peers.front().fields.sequence, 1U);
    EXPECT_EQ(*snapshot->capability, *capability);
    EXPECT_EQ(snapshot->max_lifetime, 60U);
    EXPECT_EQ(snapshot->clock_skew, 5U);
    watch->close();
    EXPECT_FALSE(watch->current_peers());
}

TEST_F(PublicWatchTest, SubscriptionChecksExactObservedRecordAndClosure) {
    start_node();
    std::optional<Advertisement> observed;
    auto callbacks = handlers();
    callbacks.on_peer = [&](Advertisement value, PeerEvent) { observed = std::move(value); };
    start_watch(std::move(callbacks));
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    ASSERT_TRUE(advance_until([&] { return observed.has_value(); }));
    EXPECT_TRUE(subscription->is_current(*observed));
    auto changed = *observed;
    changed.fields.recipient_public_key[0] = std::byte{9};
    EXPECT_FALSE(subscription->is_current(changed));
    auto wrong_group = secret();
    wrong_group[0] ^= std::byte{1};
    auto result = node->watch(wrong_group, scheduler(), {}, handlers());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
    auto other = std::move(std::get<std::unique_ptr<Subscription>>(result));
    EXPECT_FALSE(other->is_current(*observed));
    subscription->close();
    EXPECT_FALSE(subscription->is_current(*observed));
    stop_node();
}

TEST_F(PublicWatchTest, PeerCallbackMayDropItsSubscription) {
    start_node();
    auto callbacks = handlers();
    callbacks.on_peer = [this](Advertisement, PeerEvent) {
        peers.push_back(PeerEvent::Seen);
        subscription.reset();
    };
    start_watch(std::move(callbacks));
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    ASSERT_TRUE(advance_until([&] { return !peers.empty(); }));
    EXPECT_FALSE(subscription);
    stop_node();
}

TEST_F(PublicWatchTest, RejectsNotReadyAndInvalidSettings) {
    node = Node::create({}, loop.executor());
    auto result = node->watch(secret(), scheduler(), {}, handlers());
    EXPECT_EQ(std::get<WatchError>(result), WatchError::NotReady);
    start_node();
    WatchOptions options;
    options.max_pending_events = 0;
    EXPECT_THROW(static_cast<void>(node->watch(secret(), scheduler(), options, handlers())),
                 std::invalid_argument);
    stop_node();
}

TEST_F(PublicWatchTest, ClosingSuppressesQueuedAndFutureEvents) {
    start_node();
    start_watch(handlers());
    subscription->close();
    EXPECT_FALSE(subscription->healthy());
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    ASSERT_TRUE(loop.until([] { return true; }));
    EXPECT_TRUE(peers.empty());
    EXPECT_TRUE(failures.empty());
    stop_node();
}

TEST_F(PublicWatchTest, FullEventQueueReportsResourceLimit) {
    start_node();
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    AdvertisementFields fields{RecipientPublicKey{std::byte{2}},
                               InstanceId{std::byte{5}},
                               SignalingInboxKey{std::byte{4}},
                               1,
                               0,
                               time,
                               time + 20};
    auto encoded = encode_advertisement(*identity, fields, *capability, {time, 60, 5});
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    ASSERT_TRUE(publish(std::get<Bytes>(encoded)));
    WatchOptions options;
    options.max_pending_events = 1;
    auto result = node->watch(secret(), scheduler(), options, handlers());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
    subscription = std::move(std::get<std::unique_ptr<Subscription>>(result));
    ASSERT_TRUE(advance_until([&] { return !failures.empty(); }));
    EXPECT_EQ(failures.front(), WatchError::ResourceLimit);
    EXPECT_FALSE(subscription->healthy());
    stop_node();
}

TEST_F(PublicWatchTest, NodeBoundsRetainedSubscriptions) {
    start_node();
    std::vector<std::unique_ptr<Subscription>> subscriptions;
    for (int count = 0; count < 16; ++count) {
        auto result = node->watch(secret(), scheduler(), {}, handlers());
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
        subscriptions.push_back(std::move(std::get<std::unique_ptr<Subscription>>(result)));
    }
    auto excess = node->watch(secret(), scheduler(), {}, handlers());
    EXPECT_EQ(std::get<WatchError>(excess), WatchError::ResourceLimit);
    auto announce = node->announce(secret(), scheduler(), {}, {[] {}, [](auto) {}});
    EXPECT_EQ(std::get<AnnouncementError>(announce), AnnouncementError::ResourceLimit);
    stop_node();
    for (const auto &handle : subscriptions) {
        EXPECT_FALSE(handle->healthy());
    }
}

TEST_F(PublicWatchTest, ThrowingPeerHandlerMarksSubscriptionUnhealthy) {
    start_node();
    auto callbacks = handlers();
    callbacks.on_peer = [](Advertisement, PeerEvent) {
        throw std::runtime_error("handler failed");
    };
    start_watch(std::move(callbacks));
    time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    ASSERT_TRUE(advance_until([&] { return !failures.empty(); }));
    EXPECT_FALSE(subscription->healthy());
    EXPECT_EQ(failures.front(), WatchError::Callback);
    stop_node();
}

TEST_F(PublicWatchTest, SchedulerFailureIsDeferredAndReported) {
    start_node();
    Scheduler broken = [](auto, auto) -> CancelTimer { throw std::runtime_error("no timer"); };
    auto result = node->watch(secret(), std::move(broken), {}, handlers());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
    subscription = std::move(std::get<std::unique_ptr<Subscription>>(result));
    EXPECT_FALSE(subscription->healthy());
    EXPECT_TRUE(failures.empty());
    ASSERT_TRUE(loop.until([&] { return !failures.empty(); }));
    EXPECT_EQ(failures.front(), WatchError::Scheduler);
    stop_node();
}

TEST_F(PublicWatchTest, PublicNodesAnnounceAndDiscoverEachOther) {
    start_node();
    start_peer();
    ASSERT_TRUE(loop.until([&] { return first->getNodesStats(AF_INET).getKnownNodes() >= 3; }));
    start_watch(handlers());
    unsigned published = 0;
    std::vector<AnnouncementError> errors;
    auto result =
        peer_node->announce(secret(), scheduler(), {},
                            {[&] { ++published; }, [&](auto error) { errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
    auto lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    ASSERT_TRUE(advance_until([&] { return !peers.empty() && published > 0; }));
    EXPECT_EQ(peers.front(), PeerEvent::Seen);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(lease->healthy());
    stop_peer();
    EXPECT_FALSE(lease->healthy());
    stop_node();
}

TEST_F(PublicWatchTest, AnnouncementValidatesOptionsAndNodeState) {
    node = Node::create({}, loop.executor());
    AnnounceHandlers callbacks{[] {}, [](auto) {}};
    auto not_ready = node->announce(secret(), scheduler(), {}, callbacks);
    EXPECT_EQ(std::get<AnnouncementError>(not_ready), AnnouncementError::NotReady);
    start_node();
    AnnounceOptions invalid;
    invalid.max_publications = 33;
    EXPECT_THROW(static_cast<void>(node->announce(secret(), scheduler(), invalid, callbacks)),
                 std::invalid_argument);
    stop_node();
}

TEST_F(PublicWatchTest, AnnouncementStopsAtPublicationLimit) {
    start_node();
    unsigned published = 0;
    std::vector<AnnouncementError> errors;
    AnnounceOptions options;
    options.lifetime_seconds = 3;
    options.refresh_seconds = 1;
    options.max_publications = 1;
    options.protocol = DiscoveryProtocol::FixedKeyV1;
    auto result =
        node->announce(secret(), scheduler(), options,
                       {[&] { ++published; }, [&](auto error) { errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
    auto lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    ASSERT_TRUE(advance_until([&] { return !errors.empty(); }));
    EXPECT_EQ(published, 1U);
    EXPECT_EQ(errors.front(), AnnouncementError::PublicationLimit);
    EXPECT_FALSE(lease->healthy());
    stop_node();
}

TEST_F(PublicWatchTest, PublicationCallbackCanDropLease) {
    start_node();
    std::unique_ptr<Lease> lease;
    bool published = false;
    auto result = node->announce(secret(), scheduler(), {},
                                 {[&] {
                                      published = true;
                                      lease.reset();
                                  },
                                  [](auto) {}});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
    lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    ASSERT_TRUE(advance_until([&] { return published; }));
    EXPECT_FALSE(lease);
    stop_node();
}

TEST_F(PublicWatchTest, NodeBoundsAnnouncementAttemptsAfterLeaseClosure) {
    start_node();
    for (int count = 0; count < 16; ++count) {
        auto result = node->announce(secret(), scheduler(), {}, {[] {}, [](auto) {}});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
        auto lease = std::move(std::get<std::unique_ptr<Lease>>(result));
        lease->close();
        lease.reset();
        ASSERT_TRUE(loop.until([] { return true; }));
    }
    auto watch = node->watch(secret(), scheduler(), {}, handlers());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(watch));
    auto result = node->announce(secret(), scheduler(), {}, {[] {}, [](auto) {}});
    EXPECT_EQ(std::get<AnnouncementError>(result), AnnouncementError::ResourceLimit);
    stop_node();
}

TEST_F(PublicWatchTest, ThrowingPublicationHandlerMarksLeaseUnhealthy) {
    start_node();
    std::vector<AnnouncementError> errors;
    auto result = node->announce(secret(), scheduler(), {},
                                 {[] { throw std::runtime_error("publication handler failed"); },
                                  [&](auto error) { errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
    auto lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    ASSERT_TRUE(advance_until([&] { return !errors.empty(); }));
    EXPECT_FALSE(lease->healthy());
    EXPECT_EQ(errors.front(), AnnouncementError::Callback);
    stop_node();
}

TEST_F(PublicWatchTest, ClosingLeaseSuppressesDeferredSchedulerError) {
    start_node();
    Scheduler broken = [](auto, auto) -> CancelTimer { throw std::runtime_error("no timer"); };
    std::vector<AnnouncementError> errors;
    auto result = node->announce(secret(), std::move(broken), {},
                                 {[] {}, [&](auto error) { errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(result));
    auto lease = std::move(std::get<std::unique_ptr<Lease>>(result));
    EXPECT_FALSE(lease->healthy());
    lease->close();
    ASSERT_TRUE(loop.until([] { return true; }));
    EXPECT_TRUE(errors.empty());
    stop_node();
}
