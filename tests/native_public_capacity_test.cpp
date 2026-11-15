#include "../src/discovery/discovery_window.hpp"
#include "../src/discovery/native_public_watch.hpp"
#include "native_discovery_watch_fixture.hpp"

namespace {
class NativePublicCapacityTest : public NativeDiscoveryWatchTest {
  protected:
    std::vector<std::function<void()>> timers;
    std::size_t next_timer = 0;
    std::vector<PeerEvent> peers;
    std::vector<WatchError> failures;
    std::optional<bool> caught;

    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            timers.push_back(std::move(callback));
            return [] {};
        };
    }
    bool advance_until(const std::function<bool()> &condition) {
        return loop.until([&] {
            if (condition())
                return true;
            if (next_timer < timers.size())
                timers[next_timer++]();
            return false;
        });
    }
    DiscoveryHandlers handlers() {
        return {[this](Advertisement, PeerEvent event) { peers.push_back(event); },
                [this](bool ok) { caught = ok; },
                [this](WatchError error) { failures.push_back(error); }};
    }
    bool publish_peer(unsigned id) {
        AdvertisementFields fields{RecipientPublicKey{std::byte{2}},
                                   InstanceId{},
                                   SignalingInboxKey{std::byte{4}},
                                   0,
                                   0,
                                   time,
                                   time + 120};
        fields.instance_id[0] = static_cast<std::byte>(id);
        auto value = encode_advertisement(*identity, fields, *capability, {time, 300, 30});
        return std::holds_alternative<Bytes>(value) &&
               publish_windowed(std::get<Bytes>(value), time);
    }
    std::shared_ptr<DiscoveryBackend> watch(WatchOptions options) {
        DiscoveryCapability secret{};
        std::copy(capability->begin(), capability->end(), secret.begin());
        const auto records = std::clamp(options.max_peers, std::size_t{16}, std::size_t{256}) * 4;
        auto result =
            make_public_watch(NativeStore(second, {1024, records, records * 1024}), secret, scheduler(),
                              options, handlers(), loop.executor(),
                              CallbackGate(loop.executor()), MemoryBudget(4 * 1024 * 1024));
        return std::get<std::shared_ptr<DiscoveryBackend>>(std::move(result));
    }
    void set_current_time() {
        time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }
};

TEST_F(NativePublicCapacityTest, CatchesUpMoreThanSixtyFourPeers) {
    set_current_time();
    for (unsigned i = 1; i <= 80; ++i)
        ASSERT_TRUE(publish_peer(i));
    auto subscription = watch({});
    ASSERT_TRUE(advance_until([&] { return caught && peers.size() == 80; }));
    EXPECT_TRUE(*caught);
    EXPECT_TRUE(failures.empty());
    EXPECT_TRUE(subscription->healthy());
    subscription->close();
}

TEST_F(NativePublicCapacityTest, PeerCapacityExhaustionReportsIncompleteDiscovery) {
    set_current_time();
    for (unsigned i = 1; i <= 3; ++i)
        ASSERT_TRUE(publish_peer(i));
    WatchOptions options;
    options.max_peers = 2;
    auto subscription = watch(options);
    ASSERT_TRUE(advance_until([&] { return !failures.empty(); }));
    EXPECT_EQ(failures.front(), WatchError::ResourceLimit);
    EXPECT_FALSE(subscription->healthy());
    subscription->close();
}

TEST_F(NativePublicCapacityTest, RotatingProtocolFindsWindowedAnnouncements) {
    set_current_time();
    WatchOptions options;
    options.protocol = DiscoveryProtocol::RotatingKeysV2;
    auto subscription = watch(options);
    auto key = derive_discovery_window_key(*capability, discovery_window_for(time));
    auto done = std::make_shared<std::promise<bool>>();
    auto result = done->get_future();
    NativeStore writer(first, {1024, 32, 32768});
    ASSERT_FALSE(writer.put(std::get<DiscoveryKey>(key), advertisement(1, time, time + 20),
                            [done](bool ok) { done->set_value(ok); }));
    ASSERT_EQ(result.wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(result.get());
    ASSERT_TRUE(advance_until([&] { return !peers.empty() && caught.has_value(); }));
    EXPECT_TRUE(*caught);
    EXPECT_TRUE(failures.empty());
    subscription->close();
}

TEST_F(NativePublicCapacityTest, FixedKeyProtocolRemainsAvailableExplicitly) {
    set_current_time();
    EXPECT_EQ(WatchOptions{}.protocol, DiscoveryProtocol::RotatingKeysV2);
    EXPECT_EQ(AnnounceOptions{}.protocol, DiscoveryProtocol::RotatingKeysV2);
    ASSERT_TRUE(publish(advertisement(1, time, time + 20)));
    WatchOptions options;
    options.protocol = DiscoveryProtocol::FixedKeyV1;
    auto subscription = watch(options);
    ASSERT_TRUE(advance_until([&] { return !peers.empty() && caught.has_value(); }));
    EXPECT_TRUE(*caught);
    EXPECT_TRUE(failures.empty());
    subscription->close();
}

TEST_F(NativePublicCapacityTest, RotatingProtocolRejectsUnboundedTimePolicy) {
    WatchOptions options;
    options.protocol = DiscoveryProtocol::RotatingKeysV2;
    options.max_advertisement_lifetime = 841;
    options.clock_skew = 0;
    EXPECT_THROW((void)watch(options), std::invalid_argument);
    // Deliberately exercise rejection of an unknown wire/API value.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    options.protocol = static_cast<DiscoveryProtocol>(99);
    EXPECT_THROW((void)watch(options), std::invalid_argument);
    AnnounceOptions announce;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    announce.protocol = static_cast<DiscoveryProtocol>(99);
    EXPECT_THROW(
        validate_announcement(scheduler(), announce, {[] {}, [](AnnouncementError) {}}),
        std::invalid_argument);
}
} // namespace
