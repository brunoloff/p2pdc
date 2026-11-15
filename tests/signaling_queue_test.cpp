#include "../src/discovery/signaling_queue.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(SignalingQueue, PreservesOrderAndReservationsUntilRemoved) {
    MemoryBudget budget(16);
    SignalingQueue queue(budget, 16, 2, 8);
    EXPECT_FALSE(queue.front());
    EXPECT_FALSE(queue.pop());
    EXPECT_EQ(queue.push(Bytes{std::byte{1}, std::byte{2}}), SignalingQueueResult::Accepted);
    EXPECT_EQ(queue.push(Bytes{std::byte{3}}), SignalingQueueResult::Accepted);
    ASSERT_EQ(queue.size(), 2U);
    ASSERT_TRUE(queue.front());
    EXPECT_EQ(Bytes(queue.front()->begin(), queue.front()->end()),
              (Bytes{std::byte{1}, std::byte{2}}));
    EXPECT_EQ(queue.buffered_bytes(), budget.used());
    EXPECT_EQ(queue.push(Bytes{std::byte{4}}), SignalingQueueResult::Full);
    EXPECT_TRUE(queue.pop());
    ASSERT_TRUE(queue.front());
    EXPECT_EQ(Bytes(queue.front()->begin(), queue.front()->end()), (Bytes{std::byte{3}}));
    queue.close();
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(queue.push(Bytes{}), SignalingQueueResult::Closed);
}

TEST(SignalingQueue, EnforcesLimitsAndSharedBudget) {
    EXPECT_THROW((SignalingQueue(MemoryBudget(5), 5, 0, 5)), std::invalid_argument);
    EXPECT_THROW((SignalingQueue(MemoryBudget(5), 4, 1, 5)), std::invalid_argument);
    MemoryBudget budget(3);
    SignalingQueue queue(budget, 8, 4, 4);
    EXPECT_EQ(queue.push(Bytes(5)), SignalingQueueResult::TooLarge);
    EXPECT_EQ(queue.push(Bytes(2)), SignalingQueueResult::Accepted);
    EXPECT_EQ(queue.push(Bytes(2)), SignalingQueueResult::Full);
    EXPECT_TRUE(queue.pop());
    EXPECT_EQ(queue.push(Bytes(2)), SignalingQueueResult::Accepted);
    EXPECT_EQ(budget.used(), 2U);
}
