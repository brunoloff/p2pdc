#pragma once

#include "../src/discovery/discovery_window.hpp"

namespace {
void expect_window_values(NativeStore &reader, ByteView capability, std::uint64_t window,
                          std::size_t count) {
    auto done = std::make_shared<std::promise<LookupResult>>();
    auto future = done->get_future();
    auto key = std::get<DiscoveryKey>(derive_discovery_window_key(capability, window));
    ASSERT_FALSE(
        reader.lookup(key, [done](LookupResult result) { done->set_value(std::move(result)); }));
    ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.batch.error);
    EXPECT_EQ(result.batch.values.size(), count);
}

TEST_F(NativeDiscoveryWatchTest, WindowedLeaseRotatesKeysAndKeepsSequence) {
    auto config = lease_config(7, 2);
    config.windowed = true;
    std::vector<std::uint64_t> published;
    std::vector<AdvertisementLeaseError> failures;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, config, MemoryBudget(4096),
        loop.executor(), [this] { return time; },
        {[&](auto sequence) { published.push_back(sequence); },
         [&](auto error) { failures.push_back(error); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published.size() == 1; }));
    time = 110;
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published.size() == 2; }));
    time = 120;
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published.size() == 3; }));
    EXPECT_EQ(published, (std::vector<std::uint64_t>{7, 8, 9}));
    EXPECT_TRUE(failures.empty());
    NativeStore reader(second, {1024, 32, 32768});
    expect_window_values(reader, *capability, 0, 2);
    expect_window_values(reader, *capability, 1, 1);
}

TEST_F(NativeDiscoveryWatchTest, WindowedLeaseLimitsWritesWithinWindow) {
    auto config = lease_config(0, 1);
    config.windowed = true;
    std::vector<AdvertisementLeaseError> failures;
    bool published = false;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, config, MemoryBudget(4096),
        loop.executor(), [this] { return time; },
        {[&](auto) { published = true; }, [&](auto error) { failures.push_back(error); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published; }));
    time = 110;
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Failed);
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(failures.front()),
              AdvertisementLeaseFailure::PublicationLimit);
}

TEST_F(NativeDiscoveryWatchTest, WindowedLeaseFinishesPendingWriteOnOriginalKey) {
    auto config = lease_config();
    config.windowed = true;
    std::vector<std::uint64_t> published;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, config, MemoryBudget(4096),
        loop.executor(), [this] { return time; },
        {[&](auto sequence) { published.push_back(sequence); }, [](auto) { ADD_FAILURE(); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    time = 119;
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    time = 120;
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Pending);
    ASSERT_TRUE(loop.until([&] { return published.size() == 1; }));
    time = 129;
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published.size() == 2; }));
    NativeStore reader(second, {1024, 32, 32768});
    expect_window_values(reader, *capability, 0, 1);
    expect_window_values(reader, *capability, 1, 1);
}
} // namespace
