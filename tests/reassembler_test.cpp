#include "p2pdc/reassembler.h"

#include <gtest/gtest.h>

using namespace p2pdc;

namespace {
Bytes encode(const Frame &frame) {
    return std::get<Bytes>(encode_frame(frame, FrameLimits{1000, 1000}));
}
} // namespace

TEST(Reassembler, PreservesBytesAndMessageBoundaries) {
    Reassembler receiver(FrameLimits{1000, 1000});
    auto first = receiver.push(encode(Frame{42, 3, 0, {std::byte{1}, std::byte{0}}}));
    EXPECT_FALSE(first.message);
    EXPECT_FALSE(first.error);
    EXPECT_EQ(receiver.buffered_bytes(), 2U);
    auto second = receiver.push(encode(Frame{42, 3, 2, {std::byte{255}}}));
    ASSERT_TRUE(second.message);
    EXPECT_EQ(*second.message, (Bytes{std::byte{1}, std::byte{0}, std::byte{255}}));
    EXPECT_EQ(receiver.buffered_bytes(), 0U);
    auto empty = receiver.push(encode(Frame{43, 0, 0, {}}));
    ASSERT_TRUE(empty.message);
    EXPECT_TRUE(empty.message->empty());
}

TEST(Reassembler, RejectsGapsAndResetsAfterMalformedOrInterleavedFrames) {
    Reassembler receiver(FrameLimits{1000, 1000});
    auto gap = receiver.push(encode(Frame{5, 3, 1, {std::byte{1}}}));
    ASSERT_TRUE(gap.error);
    EXPECT_EQ(*gap.error, FrameError::UnexpectedFrame);
    EXPECT_FALSE(receiver.push(encode(Frame{5, 3, 0, {std::byte{1}}})).error);
    auto interleaved = receiver.push(encode(Frame{6, 1, 0, {std::byte{2}}}));
    ASSERT_TRUE(interleaved.error);
    EXPECT_EQ(*interleaved.error, FrameError::UnexpectedFrame);
    EXPECT_EQ(receiver.buffered_bytes(), 0U);
    EXPECT_FALSE(receiver.push(encode(Frame{5, 3, 0, {std::byte{1}}})).error);
    auto malformed = receiver.push(Bytes{std::byte{0}});
    ASSERT_TRUE(malformed.error);
    EXPECT_EQ(*malformed.error, FrameError::Truncated);
    EXPECT_EQ(receiver.buffered_bytes(), 0U);
    EXPECT_TRUE(receiver.push(encode(Frame{7, 1, 0, {std::byte{3}}})).message);
}

TEST(Reassembler, EnforcesLogicalMessageSize) {
    Reassembler receiver(FrameLimits{2, 1000});
    auto oversized = receiver.push(encode(Frame{2, 3, 0, {std::byte{1}}}));
    ASSERT_TRUE(oversized.error);
    EXPECT_EQ(*oversized.error, FrameError::TooLarge);
    EXPECT_EQ(receiver.buffered_bytes(), 0U);
    auto valid = receiver.push(encode(Frame{2, 2, 0, {std::byte{1}, std::byte{2}}}));
    EXPECT_TRUE(valid.message);
}

TEST(Reassembler, ResetReleasesMemoryAndRejectsOverlaps) {
    Reassembler receiver({1000, 1000});
    auto first = encode(Frame{1, 3, 0, {std::byte{1}, std::byte{2}}});
    EXPECT_FALSE(receiver.push(first).error);
    EXPECT_GE(receiver.allocated_bytes(), 2U);
    EXPECT_EQ(*receiver.push(first).error, FrameError::UnexpectedFrame);
    EXPECT_EQ(receiver.buffered_bytes(), 0U);
    EXPECT_EQ(receiver.allocated_bytes(), 0U);
    EXPECT_FALSE(receiver.push(first).error);
    receiver.reset();
    EXPECT_EQ(receiver.allocated_bytes(), 0U);
}
