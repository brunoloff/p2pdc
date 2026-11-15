#include "../src/protocol/reliable_sender.hpp"
#include "p2pdc/reassembler.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(ReliableSender, RetainsEachFrameUntilCommittedAndPreservesOrder) {
    MemoryBudget budget(100);
    ReliableSender sender({10, frame_header_size + 2}, budget, 20, 3);
    const Bytes input{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    ASSERT_EQ(sender.enqueue(input), SenderEnqueue::Accepted);
    ASSERT_EQ(sender.enqueue({}), SenderEnqueue::Accepted);
    Reassembler receiver({10, frame_header_size + 2});
    std::vector<Bytes> output;
    for (int index = 0; index < 4; ++index) {
        auto pending = sender.peek();
        ASSERT_TRUE(pending.frame);
        const Bytes snapshot(pending.frame->begin(), pending.frame->end());
        auto retried = sender.peek();
        ASSERT_TRUE(retried.frame);
        EXPECT_EQ(Bytes(retried.frame->begin(), retried.frame->end()), snapshot);
        auto decoded = receiver.push(snapshot);
        ASSERT_FALSE(decoded.error);
        if (decoded.message) {
            output.push_back(std::move(*decoded.message));
        }
        sender.commit();
    }
    ASSERT_EQ(output.size(), 2U);
    EXPECT_EQ(output[0], input);
    EXPECT_TRUE(output[1].empty());
    EXPECT_EQ(sender.queued_bytes(), 0U);
    EXPECT_EQ(sender.queued_messages(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_FALSE(sender.peek().frame);
    EXPECT_THROW(sender.commit(), std::logic_error);
}

TEST(ReliableSender, RejectsAtomicallyAndReleasesMemoryOnClear) {
    MemoryBudget budget(30);
    ReliableSender sender({6, 21}, budget, 6, 2);
    EXPECT_EQ(sender.enqueue(Bytes(7)), SenderEnqueue::TooLarge);
    EXPECT_EQ(sender.enqueue(Bytes(6)), SenderEnqueue::Accepted);
    EXPECT_EQ(sender.enqueue(Bytes(1)), SenderEnqueue::Backpressure);
    EXPECT_EQ(sender.queued_bytes(), 6U);
    ASSERT_TRUE(sender.peek().frame);
    sender.commit();
    EXPECT_EQ(sender.queued_messages(), 1U);
    EXPECT_EQ(sender.queued_bytes(), 6U);
    sender.clear();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(sender.queued_messages(), 0U);
    EXPECT_EQ(sender.enqueue(Bytes(1)), SenderEnqueue::Closed);
    EXPECT_FALSE(sender.peek().frame);
}

TEST(ReliableSender, ReservesScratchBeforeAdmittingMessages) {
    MemoryBudget budget(25);
    ReliableSender first({4, 21}, budget, 4, 1), second({4, 21}, budget, 4, 1);
    EXPECT_TRUE(first.can_enqueue(4));
    EXPECT_FALSE(first.can_enqueue(5));
    EXPECT_EQ(first.enqueue(Bytes(4)), SenderEnqueue::Accepted);
    EXPECT_EQ(budget.used(), 25U);
    EXPECT_EQ(second.enqueue({}), SenderEnqueue::Backpressure);
    EXPECT_FALSE(second.can_enqueue(0));
    ASSERT_TRUE(first.peek().frame);
    first.commit();
    ASSERT_TRUE(first.peek().frame);
    first.commit();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_TRUE(second.can_enqueue(0));
    EXPECT_EQ(second.enqueue({}), SenderEnqueue::Accepted);
    EXPECT_EQ(second.enqueue({}), SenderEnqueue::Backpressure);
    EXPECT_TRUE(second.peek().frame);
}

TEST(ReliableSender, ValidatesConfiguration) {
    MemoryBudget budget(100);
    EXPECT_THROW((ReliableSender{{1, frame_header_size}, budget, 1, 1}), std::invalid_argument);
    EXPECT_THROW((ReliableSender{{2, 20}, budget, 1, 1}), std::invalid_argument);
    EXPECT_THROW((ReliableSender{{1, 20}, budget, 1, 0}), std::invalid_argument);
}
