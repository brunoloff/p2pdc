#include "../src/protocol/reliable_receiver.hpp"

#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
Bytes frame(std::uint64_t id, std::uint32_t total, std::uint32_t offset, Bytes payload) {
    return std::get<Bytes>(encode_frame(Frame{id, total, offset, std::move(payload)}, {16, 64}));
}
} // namespace

TEST(ReliableReceiver, PreservesMessageAndReservationThroughDelivery) {
    MemoryBudget budget(8);
    ReliableReceiver receiver({16, 64}, budget);
    auto first = receiver.push(frame(1, 3, 0, {std::byte{1}, std::byte{0}}));
    EXPECT_FALSE(first.error);
    EXPECT_FALSE(first.message);
    EXPECT_GE(budget.used(), 3U);
    auto last = receiver.push(frame(1, 3, 2, {std::byte{255}}));
    ASSERT_TRUE(last.message);
    EXPECT_EQ(last.message->bytes, (Bytes{std::byte{1}, std::byte{0}, std::byte{255}}));
    EXPECT_GE(budget.used(), 3U);
    receiver.reset();
    EXPECT_GE(budget.used(), 3U);
    last.message.reset();
    EXPECT_EQ(budget.used(), 0U);
    auto empty = receiver.push(frame(2, 0, 0, {}));
    ASSERT_TRUE(empty.message);
    EXPECT_TRUE(empty.message->bytes.empty());
}

TEST(ReliableReceiver, SharesBudgetAndReleasesOnMalformedInput) {
    MemoryBudget budget(4);
    ReliableReceiver first({16, 64}, budget), second({16, 64}, budget);
    EXPECT_FALSE(first.push(frame(1, 4, 0, {std::byte{1}})).error);
    EXPECT_EQ(budget.used(), 4U);
    auto rejected = second.push(frame(2, 1, 0, {std::byte{2}}));
    ASSERT_TRUE(rejected.error);
    EXPECT_EQ(*rejected.error, ReceiveError::ResourceLimit);
    auto malformed = first.push(Bytes{std::byte{0}});
    ASSERT_TRUE(malformed.error);
    EXPECT_EQ(*malformed.error, ReceiveError::InvalidFrame);
    EXPECT_EQ(malformed.frame_error, FrameError::Truncated);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_TRUE(second.push(frame(2, 1, 0, {std::byte{2}})).message);
}

TEST(ReliableReceiver, RejectsUnexpectedFramesAndCanBeReset) {
    MemoryBudget budget(8);
    ReliableReceiver receiver({16, 64}, budget);
    auto gap = receiver.push(frame(1, 2, 1, {std::byte{1}}));
    EXPECT_EQ(gap.error, ReceiveError::InvalidFrame);
    EXPECT_EQ(gap.frame_error, FrameError::UnexpectedFrame);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_FALSE(receiver.push(frame(1, 2, 0, {std::byte{1}})).error);
    auto mismatch = receiver.push(frame(2, 2, 1, {std::byte{2}}));
    EXPECT_EQ(mismatch.error, ReceiveError::InvalidFrame);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_FALSE(receiver.push(frame(3, 2, 0, {std::byte{1}})).error);
    receiver.reset();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ReliableReceiver, RetainedMessageBlocksAnotherUntilReleased) {
    MemoryBudget budget(2);
    ReliableReceiver receiver({16, 64}, budget);
    auto held = receiver.push(frame(1, 2, 0, {std::byte{1}, std::byte{2}}));
    ASSERT_TRUE(held.message);
    auto blocked = receiver.push(frame(2, 1, 0, {std::byte{3}}));
    EXPECT_EQ(blocked.error, ReceiveError::ResourceLimit);
    held.message.reset();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_TRUE(receiver.push(frame(2, 1, 0, {std::byte{3}})).message);
    for (int index = 0; index < 2; ++index) {
        auto empty = receiver.push(frame(3, 0, 0, {}));
        ASSERT_TRUE(empty.message);
        EXPECT_TRUE(empty.message->bytes.empty());
        EXPECT_EQ(budget.used(), 0U);
    }
}
