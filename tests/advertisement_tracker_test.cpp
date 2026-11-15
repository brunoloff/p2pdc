#include "../src/discovery/advertisement_tracker.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
Advertisement record(std::uint64_t sequence, std::uint64_t issued, std::uint64_t expiry) {
    return {SigningPublicKey{std::byte{1}},
            {RecipientPublicKey{std::byte{2}}, InstanceId{std::byte{3}},
             SignalingInboxKey{std::byte{4}}, sequence, 0, issued, expiry}};
}
} // namespace

TEST(AdvertisementTracker, TracksUpdatesAndRetainsWatermarkAfterExpiry) {
    AdvertisementTracker tracker(2, 60, 5);
    auto old = record(1, 100, 160);
    auto recent = record(2, 101, 102);
    EXPECT_EQ(tracker.observe(old, 100), AdvertisementEvent::Seen);
    EXPECT_EQ(tracker.observe(old, 100), AdvertisementEvent::Duplicate);
    EXPECT_EQ(tracker.observe(recent, 101), AdvertisementEvent::Updated);
    auto conflicting = recent;
    conflicting.fields.features = 1;
    EXPECT_EQ(tracker.observe(conflicting, 101), AdvertisementEvent::Conflict);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.sweep(106)).empty());
    EXPECT_EQ(std::get<std::vector<Advertisement>>(tracker.sweep(107)).size(), 1U);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.sweep(107)).empty());
    EXPECT_EQ(tracker.observe(old, 107), AdvertisementEvent::Stale);
    EXPECT_EQ(tracker.size(), 1U);
    auto newest = record(3, 108, 150);
    EXPECT_EQ(tracker.observe(newest, 108), AdvertisementEvent::Seen);
    auto backdated = record(4, 107, 150);
    EXPECT_EQ(tracker.observe(backdated, 108), AdvertisementEvent::Invalid);
}

TEST(AdvertisementTracker, BoundsTombstonesAndRejectsClockRegression) {
    EXPECT_THROW((AdvertisementTracker(0, 60, 5)), std::invalid_argument);
    EXPECT_THROW((AdvertisementTracker(1, 0, 5)), std::invalid_argument);
    AdvertisementTracker tracker(1, 60, 5);
    auto first = record(1, 100, 101);
    auto second = record(1, 100, 150);
    second.fields.instance_id[0] = std::byte{9};
    EXPECT_EQ(tracker.observe(first, 100), AdvertisementEvent::Seen);
    EXPECT_EQ(tracker.observe(second, 100), AdvertisementEvent::Full);
    EXPECT_EQ(std::get<std::vector<Advertisement>>(tracker.sweep(106)).size(), 1U);
    EXPECT_EQ(tracker.observe(second, 106), AdvertisementEvent::Full);
    EXPECT_EQ(std::get<AdvertisementSweepError>(tracker.sweep(105)),
              AdvertisementSweepError::ClockRegressed);
    EXPECT_EQ(tracker.observe(second, 105), AdvertisementEvent::ClockRegressed);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.sweep(171)).empty());
    EXPECT_EQ(tracker.size(), 0U);
}

TEST(AdvertisementTracker, RejectsInvalidFieldsTimesAndOverflowEviction) {
    AdvertisementTracker tracker(2, 60, 5);
    auto invalid = record(1, 100, 150);
    invalid.fields.signaling_inbox = {};
    EXPECT_EQ(tracker.observe(invalid, 100), AdvertisementEvent::Invalid);
    EXPECT_EQ(tracker.observe(record(1, 106, 150), 100), AdvertisementEvent::Invalid);
    EXPECT_EQ(tracker.observe(record(1, 100, 151), 156), AdvertisementEvent::Invalid);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto near_end = record(1, maximum - 10, maximum - 1);
    EXPECT_EQ(tracker.observe(near_end, maximum - 10), AdvertisementEvent::Seen);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.sweep(maximum)).empty());
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(AdvertisementTracker, RetainsFutureIssuedWatermarkThroughSkewBoundary) {
    AdvertisementTracker tracker(1, 60, 5);
    auto future = record(1, 105, 165);
    EXPECT_EQ(tracker.observe(future, 100), AdvertisementEvent::Seen);
    EXPECT_EQ(std::get<std::vector<Advertisement>>(tracker.sweep(170)).size(), 1U);
    EXPECT_EQ(tracker.size(), 1U);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.sweep(171)).empty());
    EXPECT_EQ(tracker.size(), 0U);
}

TEST(AdvertisementTracker, CurrentRecordRequiresExactVisibleUnexpiredMatch) {
    AdvertisementTracker tracker(2, 60, 5);
    auto first = record(1, 100, 150);
    EXPECT_FALSE(std::get<bool>(tracker.is_current(first, 100)));
    ASSERT_EQ(tracker.observe(first, 100), AdvertisementEvent::Seen);
    EXPECT_TRUE(std::get<bool>(tracker.is_current(first, 100)));
    auto changed = first;
    changed.fields.recipient_public_key[0] = std::byte{7};
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    changed.fields.signaling_inbox[0] = std::byte{8};
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    changed.signing_public_key[0] = std::byte{8};
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    changed.fields.instance_id[0] = std::byte{8};
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    ++changed.fields.sequence;
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    ++changed.fields.features;
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    --changed.fields.issued_at;
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    changed = first;
    --changed.fields.expires_at;
    EXPECT_FALSE(std::get<bool>(tracker.is_current(changed, 100)));
    auto newer = record(2, 101, 120);
    ASSERT_EQ(tracker.observe(newer, 101), AdvertisementEvent::Updated);
    EXPECT_FALSE(std::get<bool>(tracker.is_current(first, 101)));
    EXPECT_TRUE(std::get<bool>(tracker.is_current(newer, 124)));
    EXPECT_FALSE(std::get<bool>(tracker.is_current(newer, 125)));
    EXPECT_EQ(std::get<std::vector<Advertisement>>(tracker.sweep(125)).size(), 1U);
    EXPECT_EQ(std::get<AdvertisementSweepError>(tracker.is_current(newer, 124)),
              AdvertisementSweepError::ClockRegressed);
}

TEST(AdvertisementTracker, SweptAndUnknownRecordsAreNotCurrent) {
    AdvertisementTracker tracker(2, 60, 5);
    auto first = record(1, 100, 101);
    ASSERT_EQ(tracker.observe(first, 100), AdvertisementEvent::Seen);
    ASSERT_EQ(std::get<std::vector<Advertisement>>(tracker.sweep(106)).size(), 1U);
    EXPECT_FALSE(std::get<bool>(tracker.is_current(first, 106)));
    auto other = record(1, 106, 120);
    other.fields.instance_id[0] = std::byte{9};
    EXPECT_FALSE(std::get<bool>(tracker.is_current(other, 106)));
}

TEST(AdvertisementTracker, SnapshotContainsOnlyCurrentValuesWithoutSweeping) {
    AdvertisementTracker tracker(3, 60, 5);
    auto first = record(1, 100, 120);
    auto second = record(1, 100, 101);
    second.fields.instance_id[0] = std::byte{9};
    ASSERT_EQ(tracker.observe(first, 100), AdvertisementEvent::Seen);
    ASSERT_EQ(tracker.observe(second, 100), AdvertisementEvent::Seen);
    auto updated = record(2, 101, 140);
    ASSERT_EQ(tracker.observe(updated, 101), AdvertisementEvent::Updated);
    EXPECT_EQ(std::get<std::vector<Advertisement>>(tracker.current_peers(105)).size(), 2U);
    auto snapshot = std::get<std::vector<Advertisement>>(tracker.current_peers(106));
    ASSERT_EQ(snapshot.size(), 1U);
    EXPECT_EQ(snapshot.front(), updated);
    EXPECT_EQ(std::get<std::vector<Advertisement>>(tracker.sweep(106)).size(), 1U);
    EXPECT_EQ(tracker.observe(second, 106), AdvertisementEvent::Invalid);
    EXPECT_EQ(std::get<AdvertisementSweepError>(tracker.current_peers(105)),
              AdvertisementSweepError::ClockRegressed);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.current_peers(145)).empty());
}

TEST(AdvertisementTracker, EmptySnapshotStillRejectsClockRegression) {
    AdvertisementTracker tracker(1, 60, 5);
    EXPECT_TRUE(std::get<std::vector<Advertisement>>(tracker.current_peers(100)).empty());
    EXPECT_EQ(std::get<AdvertisementSweepError>(tracker.current_peers(99)),
              AdvertisementSweepError::ClockRegressed);
}
