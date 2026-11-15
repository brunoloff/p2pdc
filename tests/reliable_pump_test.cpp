#include "../src/protocol/reliable_pump.hpp"
#include "p2pdc/reassembler.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
class TestSink : public ReliableSink {
  public:
    std::size_t buffered = 0;
    SinkResult result = SinkResult::Accepted;
    std::vector<Bytes> frames;
    std::size_t buffered_amount() const override { return buffered; }
    SinkResult send(ByteView frame) override {
        if (result == SinkResult::Accepted) {
            frames.emplace_back(frame.begin(), frame.end());
            buffered += frame.size();
        }
        return result;
    }
};
} // namespace

TEST(ReliablePump, HonorsHighWaterAndRetriesTheSameFrame) {
    MemoryBudget budget(100);
    ReliableSender sender({8, 21}, budget, 8, 1);
    TestSink sink;
    ReliablePump pump(sender, sink, 21, 3);
    ASSERT_EQ(sender.enqueue(Bytes{std::byte{1}, std::byte{2}, std::byte{3}}),
              SenderEnqueue::Accepted);
    EXPECT_EQ(pump.run(), PumpResult::Blocked);
    ASSERT_EQ(sink.frames.size(), 1U);
    const Bytes second(sender.peek().frame->begin(), sender.peek().frame->end());
    sink.result = SinkResult::NotReady;
    sink.buffered = 0;
    EXPECT_EQ(pump.run(), PumpResult::Blocked);
    EXPECT_EQ(Bytes(sender.peek().frame->begin(), sender.peek().frame->end()), second);
    sink.result = SinkResult::Accepted;
    EXPECT_EQ(pump.run(), PumpResult::Idle);
    ASSERT_EQ(sink.frames.size(), 2U);
    EXPECT_EQ(sink.frames.back(), second);
    Reassembler receiver({8, 21});
    EXPECT_FALSE(receiver.push(sink.frames[0]).message);
    auto output = receiver.push(sink.frames[1]);
    ASSERT_TRUE(output.message);
    EXPECT_EQ(*output.message, (Bytes{std::byte{1}, std::byte{2}, std::byte{3}}));
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ReliablePump, LimitsWorkPerPassAndHandlesEmptyQueue) {
    MemoryBudget budget(100);
    ReliableSender sender({8, 20}, budget, 8, 1);
    TestSink sink;
    ReliablePump pump(sender, sink, 100, 2);
    EXPECT_EQ(pump.run(), PumpResult::Idle);
    ASSERT_EQ(sender.enqueue(Bytes(3)), SenderEnqueue::Accepted);
    EXPECT_EQ(pump.run(), PumpResult::More);
    EXPECT_EQ(sink.frames.size(), 2U);
    EXPECT_EQ(pump.run(), PumpResult::Idle);
    EXPECT_EQ(sink.frames.size(), 3U);
}

TEST(ReliablePump, TerminalFailureReleasesPendingState) {
    MemoryBudget budget(100);
    ReliableSender sender({8, 21}, budget, 8, 1);
    TestSink sink;
    ReliablePump pump(sender, sink, 21, 1);
    ASSERT_EQ(sender.enqueue(Bytes(3)), SenderEnqueue::Accepted);
    sink.result = SinkResult::Failed;
    EXPECT_EQ(pump.run(), PumpResult::Failed);
    EXPECT_EQ(pump.run(), PumpResult::Failed);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(sender.enqueue({}), SenderEnqueue::Closed);
}

TEST(ReliablePump, RejectsImpossibleFramesAndInvalidLimits) {
    MemoryBudget budget(100);
    ReliableSender sender({8, 21}, budget, 8, 1);
    TestSink sink;
    EXPECT_THROW((ReliablePump{sender, sink, 18, 1}), std::invalid_argument);
    EXPECT_THROW((ReliablePump{sender, sink, 21, 0}), std::invalid_argument);
    ReliablePump pump(sender, sink, 20, 1);
    ASSERT_EQ(sender.enqueue(Bytes(2)), SenderEnqueue::Accepted);
    EXPECT_EQ(pump.run(), PumpResult::Failed);
    EXPECT_TRUE(sink.frames.empty());
    EXPECT_EQ(budget.used(), 0U);
}
