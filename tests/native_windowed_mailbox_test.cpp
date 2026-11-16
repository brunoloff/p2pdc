#include "../src/discovery/native_windowed_mailbox.hpp"
#include "native_discovery_watch_fixture.hpp"

namespace {
class NativeWindowedMailboxTest : public NativeDiscoveryWatchTest {};

bool put_window(NativeStore &store, ByteView capability, std::uint64_t window, ByteView wire) {
    auto key = std::get<DiscoveryKey>(derive_discovery_window_key(capability, window));
    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    if (store.put(key, wire, [done](bool success) { done->set_value(success); }))
        return false;
    return future.wait_for(10s) == std::future_status::ready && future.get();
}

void track_record(ByteView wire, ByteView capability, std::uint64_t now,
                  AdvertisementTracker &tracker, std::vector<AdvertisementEvent> &observed) {
    auto decoded = decode_advertisement(wire, capability, {now, 60, 5});
    if (auto *value = std::get_if<Advertisement>(&decoded)) {
        auto event = tracker.observe(*value, now);
        if (event == AdvertisementEvent::Seen || event == AdvertisementEvent::Updated)
            observed.push_back(event);
    }
}

TEST_F(NativeWindowedMailboxTest, CatchesUpAndSharesReplayState) {
    time = 110;
    NativeStore writer(first, {1024, 32, 32768});
    auto old_wire = advertisement(7, 110, 130);
    ASSERT_TRUE(put_window(writer, *capability, 0, old_wire));
    AdvertisementTracker tracker(4, 60, 5);
    std::vector<AdvertisementEvent> observed;
    std::vector<WindowedMailboxError> failures;
    std::vector<bool> caught;
    NativeWindowedMailbox mailbox(
        NativeStore(second, {1024, 32, 32768}), capability, {4096, 16, 1024}, 60, 5,
        MemoryBudget(8192), loop.executor(),
        {[&](Bytes wire) { track_record(wire, *capability, time, tracker, observed); },
         [&](auto error, auto) { failures.push_back(error); },
         [&](bool success) { caught.push_back(success); }});
    ASSERT_TRUE(mailbox.tick(time));
    ASSERT_TRUE(
        loop.until([&] { return mailbox.tick(time) && !caught.empty() && !observed.empty(); }));
    ASSERT_TRUE(caught.front());
    EXPECT_EQ(observed.front(), AdvertisementEvent::Seen);
    time = 120;
    ASSERT_TRUE(mailbox.tick(time));
    auto new_wire = advertisement(8, 120, 140);
    ASSERT_TRUE(put_window(writer, *capability, 1, new_wire));
    ASSERT_TRUE(loop.until(
        [&] { return mailbox.tick(time) && caught.size() == 2 && observed.size() == 2; }));
    EXPECT_TRUE(caught.back());
    EXPECT_EQ(observed.back(), AdvertisementEvent::Updated);
    auto old = decode_advertisement(old_wire, *capability, {time, 60, 5});
    ASSERT_TRUE(std::holds_alternative<Advertisement>(old));
    EXPECT_EQ(tracker.observe(std::get<Advertisement>(old), time), AdvertisementEvent::Stale);
    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(observed.size(), 2U);
}

TEST_F(NativeWindowedMailboxTest, RejectsClockRollbackAndOversizedRange) {
    std::vector<WindowedMailboxError> failures;
    auto handlers = WindowedMailboxCallbacks{
        [](Bytes) {}, [&](auto error, auto) { failures.push_back(error); }, [](bool) {}};
    NativeStore store(first, {1024, 32, 32768});
    NativeWindowedMailbox mailbox(store, capability, {4096, 16, 1024}, 60, 5, MemoryBudget(8192),
                                  loop.executor(), handlers);
    ASSERT_TRUE(mailbox.tick(120));
    EXPECT_FALSE(mailbox.tick(119));
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures.back(), WindowedMailboxError::ClockRegressed);
    failures.clear();
    NativeWindowedMailbox oversized(store, capability, {4096, 16, 1024}, 2000, 5,
                                    MemoryBudget(8192), loop.executor(), handlers);
    EXPECT_FALSE(oversized.tick(3000));
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures.back(), WindowedMailboxError::InvalidRange);
}

TEST_F(NativeWindowedMailboxTest, ReportsStoppedStore) {
    std::vector<WindowedMailboxError> failures;
    first->join();
    NativeWindowedMailbox mailbox(
        NativeStore(first, {1024, 32, 32768}), capability, {4096, 16, 1024}, 60, 5,
        MemoryBudget(8192), loop.executor(),
        {[](Bytes) {}, [&](auto error, auto) { failures.push_back(error); }, [](bool) {}});
    EXPECT_FALSE(mailbox.tick(time));
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures.front(), WindowedMailboxError::Store);
}

TEST_F(NativeWindowedMailboxTest, DiscoveryWatchTracksLeaseAcrossWindows) {
    time = 110;
    DiscoveryWatchLimits bounds{{4096, 16, 1024}, 4, 60, 5, true};
    auto created = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, bounds, MemoryBudget(8192),
        loop.executor(), [this] { return time; }, callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeDiscoveryWatch>>(created));
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(created));
    EXPECT_TRUE(watch->automatically_catches_up());
    EXPECT_FALSE(watch->registration().valid());
    auto config = lease_config();
    config.windowed = true;
    std::vector<std::uint64_t> published;
    auto lease_created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, config, MemoryBudget(4096),
        loop.executor(), [this] { return time; },
        {[&](auto sequence) { published.push_back(sequence); }, [](auto) { ADD_FAILURE(); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(lease_created));
    ASSERT_TRUE(watch->tick());
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until(
        [&] { return watch->tick() && caught_up && events.size() == 1 && published.size() == 1; }));
    EXPECT_TRUE(*caught_up);
    EXPECT_EQ(events[0], AdvertisementEvent::Seen);
    time = 120;
    caught_up.reset();
    ASSERT_TRUE(watch->tick());
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until(
        [&] { return watch->tick() && caught_up && events.size() == 2 && published.size() == 2; }));
    EXPECT_TRUE(*caught_up);
    EXPECT_EQ(events[1], AdvertisementEvent::Updated);
    EXPECT_TRUE(errors.empty());
}

TEST_F(NativeWindowedMailboxTest, DiscoveryWatchDropsRecordsWhenQueueBudgetIsExhausted) {
    for (const bool windowed : {false, true}) {
        errors.clear();
        events.clear();
        DiscoveryWatchLimits bounds{{4096, 16, 1024}, 4, 60, 5, windowed};
        auto created = NativeDiscoveryWatch::create(
            NativeStore(second, {1024, 32, 32768}), capability, bounds, MemoryBudget(1),
            loop.executor(), [this] { return time; }, callbacks());
        auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(created));
        if (windowed) {
            ASSERT_TRUE(watch->tick());
        } else {
            ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
            ASSERT_NE(watch->registration().get(), 0U);
        }
        auto key = windowed ? derive_discovery_window_key(*capability, 0)
                            : derive_discovery_key(*capability);
        NativeStore writer(first, {1024, 32, 32768});
        auto done = std::make_shared<std::promise<bool>>();
        auto result = done->get_future();
        ASSERT_FALSE(writer.put(std::get<DiscoveryKey>(key), advertisement(0, time, time + 20),
                                [done](bool ok) { done->set_value(ok); }));
        ASSERT_EQ(result.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(result.get());
        const auto deadline = std::chrono::steady_clock::now() + 300ms;
        ASSERT_TRUE(loop.until([&] {
            if (watch->is_open())
                (void)watch->tick();
            return std::chrono::steady_clock::now() >= deadline;
        }));
        EXPECT_TRUE(watch->is_open());
        EXPECT_TRUE(errors.empty());
        EXPECT_TRUE(events.empty());
        watch->close();
    }
}
} // namespace
