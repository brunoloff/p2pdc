#include "p2pdc/fragmenter.h"
#include "p2pdc/reassembler.h"

#include <gtest/gtest.h>
#include <type_traits>

using namespace p2pdc;

static_assert(!std::is_move_constructible_v<Fragmenter>);

TEST(Fragmenter, LargeMessageRoundTrips) {
    Bytes message(4 * 1024 * 1024, std::byte{0xa5});
    FrameLimits limits{static_cast<std::uint32_t>(message.size()), 16384};
    Fragmenter sender(message, 77, limits);
    Reassembler receiver(limits);
    std::optional<Bytes> result;
    for (;;) {
        auto next = sender.next();
        ASSERT_FALSE(next.error);
        if (!next.frame) {
            break;
        }
        EXPECT_LE(next.frame->size(), limits.max_frame_size);
        auto reassembled = receiver.push(*next.frame);
        ASSERT_FALSE(reassembled.error);
        if (reassembled.message) {
            result = std::move(reassembled.message);
        }
    }
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, message);
    EXPECT_EQ(sender.allocated_bytes(), 0U);
}

TEST(Fragmenter, EmptyAndInvalidLimits) {
    Fragmenter empty({}, 0, {0, frame_header_size});
    auto frame = empty.next();
    ASSERT_TRUE(frame.frame);
    EXPECT_EQ(std::get<Frame>(decode_frame(*frame.frame, {0, frame_header_size})).total_size, 0U);
    EXPECT_FALSE(empty.next().frame);
    Fragmenter no_room({std::byte{1}}, 0, {1, frame_header_size});
    EXPECT_EQ(no_room.next().error, FrameError::TooLarge);
    EXPECT_FALSE(no_room.next().error);
    Fragmenter too_large({std::byte{1}}, 0, {0, 100});
    EXPECT_EQ(too_large.next().error, FrameError::TooLarge);
    EXPECT_EQ(too_large.allocated_bytes(), 0U);
}

TEST(Fragmenter, ExactBoundariesAndSingleBytePayloads) {
    const Bytes input{std::byte{1}, std::byte{2}};
    Fragmenter single_byte(input, 4, {2, frame_header_size + 1});
    EXPECT_EQ(std::get<Frame>(decode_frame(*single_byte.next().frame, {2, frame_header_size + 1}))
                  .payload.size(),
              1U);
    EXPECT_EQ(
        std::get<Frame>(decode_frame(*single_byte.next().frame, {2, frame_header_size + 1})).offset,
        1U);
    EXPECT_FALSE(single_byte.next().frame);
    Fragmenter exact(input, 5, {2, frame_header_size + 2});
    EXPECT_EQ(
        std::get<Frame>(decode_frame(*exact.next().frame, {2, frame_header_size + 2})).payload,
        input);
    EXPECT_FALSE(exact.next().frame);
}
