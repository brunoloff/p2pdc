#include "p2pdc/send_queue.h"

#include <gtest/gtest.h>
#include <type_traits>

using namespace p2pdc;

static_assert(!std::is_move_constructible_v<SendQueue>);

TEST(SendQueue, BoundedBytesAndOrdering) {
    SendQueue queue(3, 3, 2);
    EXPECT_EQ(queue.enqueue(Bytes{std::byte{1}, std::byte{2}}), EnqueueResult::Accepted);
    EXPECT_EQ(queue.enqueue(Bytes{std::byte{3}, std::byte{4}}), EnqueueResult::Backpressure);
    EXPECT_EQ(queue.enqueue(Bytes{std::byte{3}}), EnqueueResult::Accepted);
    EXPECT_EQ(queue.buffered_bytes(), 3U);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(*queue.pop(), (Bytes{std::byte{1}, std::byte{2}}));
    EXPECT_EQ(*queue.pop(), (Bytes{std::byte{3}}));
    EXPECT_FALSE(queue.pop());
    EXPECT_EQ(queue.buffered_bytes(), 0U);
}

TEST(SendQueue, RejectsInvalidLimitsAndBoundsEmptyMessages) {
    EXPECT_THROW((SendQueue{1, 0, 1}), std::invalid_argument);
    EXPECT_THROW((SendQueue{1, 1, 2}), std::invalid_argument);
    SendQueue queue(1, 1, 1);
    EXPECT_EQ(queue.enqueue(Bytes{std::byte{1}, std::byte{2}}), EnqueueResult::TooLarge);
    EXPECT_EQ(queue.enqueue(Bytes{}), EnqueueResult::Accepted);
    EXPECT_EQ(queue.enqueue(Bytes{}), EnqueueResult::Backpressure);
    ASSERT_TRUE(queue.pop());
    EXPECT_EQ(queue.enqueue(Bytes{}), EnqueueResult::Accepted);
}

TEST(SendQueue, TakesCopyAndAllowsRetryAfterBackpressure) {
    SendQueue queue(1, 1, 1);
    Bytes input{std::byte{3}};
    EXPECT_EQ(queue.enqueue(input), EnqueueResult::Accepted);
    input[0] = std::byte{4};
    EXPECT_EQ(queue.enqueue(input), EnqueueResult::Backpressure);
    EXPECT_EQ(*queue.pop(), (Bytes{std::byte{3}}));
    EXPECT_EQ(queue.enqueue(input), EnqueueResult::Accepted);
    EXPECT_EQ(*queue.pop(), input);
}
