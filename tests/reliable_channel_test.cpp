#include "../src/protocol/reliable_channel.hpp"

#include <gtest/gtest.h>

#include <deque>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Loop {
    std::deque<std::function<void()>> tasks;
    Executor executor() {
        return [this](auto task) { tasks.push_back(std::move(task)); };
    }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
};
struct Sink : ReliableSink {
    SinkResult result = SinkResult::Accepted;
    std::vector<Bytes> frames;
    std::size_t buffered_amount() const override { return 0; }
    SinkResult send(ByteView frame) override {
        if (result == SinkResult::Accepted) {
            frames.emplace_back(frame.begin(), frame.end());
        }
        return result;
    }
};
struct Fixture {
    Loop loop;
    Sink sink;
    std::vector<Bytes> received;
    std::vector<ChannelFailure> errors;
    int writable = 0;
    ReliableChannelCallbacks callbacks() {
        return {[this](Bytes bytes) { received.push_back(std::move(bytes)); },
                [this](ChannelFailure error) { errors.push_back(error); }, [this] { ++writable; }};
    }
    ReliableChannelLimits limits() { return {{8, 21}, 8, 1, 64, 1}; }
};
} // namespace

TEST(ReliableChannel, SendsInFairPassesAndReassemblesMessages) {
    Fixture f;
    ReliableChannel channel(f.limits(), MemoryBudget(100), f.sink, f.loop.executor(),
                            f.callbacks());
    EXPECT_EQ(channel.send({}), ChannelSend::NotReady);
    channel.set_ready(true);
    EXPECT_EQ(channel.send(Bytes{std::byte{1}, std::byte{2}, std::byte{3}}), ChannelSend::Accepted);
    EXPECT_TRUE(f.sink.frames.empty());
    f.loop.drain();
    ASSERT_EQ(f.sink.frames.size(), 2U);
    for (const auto &frame : f.sink.frames) {
        channel.on_frame(frame);
    }
    ASSERT_EQ(f.received.size(), 1U);
    EXPECT_EQ(f.received[0], (Bytes{std::byte{1}, std::byte{2}, std::byte{3}}));
    EXPECT_TRUE(f.errors.empty());
}

TEST(ReliableChannel, SuspendsRetriesAndSignalsWritable) {
    Fixture f;
    ReliableChannel *pointer = nullptr;
    ChannelSend retry = ChannelSend::Failed;
    auto callbacks = f.callbacks();
    callbacks.on_writable = [&] {
        ++f.writable;
        retry = pointer->send(Bytes(1));
    };
    ReliableChannel channel(f.limits(), MemoryBudget(100), f.sink, f.loop.executor(),
                            std::move(callbacks));
    pointer = &channel;
    channel.set_ready(true);
    f.sink.result = SinkResult::NotReady;
    EXPECT_EQ(channel.send(Bytes(3)), ChannelSend::Accepted);
    EXPECT_EQ(channel.send(Bytes(1)), ChannelSend::Backpressure);
    f.loop.drain();
    EXPECT_TRUE(f.sink.frames.empty());
    EXPECT_EQ(f.writable, 0);
    channel.set_ready(false);
    EXPECT_EQ(channel.send({}), ChannelSend::NotReady);
    f.sink.result = SinkResult::Accepted;
    channel.set_ready(true);
    f.loop.drain();
    EXPECT_EQ(f.sink.frames.size(), 3U);
    EXPECT_EQ(f.writable, 1);
    EXPECT_EQ(retry, ChannelSend::Accepted);
    EXPECT_EQ(channel.send({}), ChannelSend::Accepted);
}

TEST(ReliableChannel, MalformedFrameTerminatesAndCloseSuppressesQueuedWork) {
    Fixture f;
    MemoryBudget budget(100);
    ReliableChannel channel(f.limits(), budget, f.sink, f.loop.executor(), f.callbacks());
    channel.set_ready(true);
    EXPECT_EQ(channel.send(Bytes(2)), ChannelSend::Accepted);
    channel.on_frame(Bytes{std::byte{0}});
    ASSERT_EQ(f.errors.size(), 1U);
    EXPECT_EQ(f.errors[0], ChannelFailure::InvalidFrame);
    EXPECT_EQ(channel.send({}), ChannelSend::Closed);
    f.loop.drain();
    EXPECT_TRUE(f.sink.frames.empty());
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ReliableChannel, ReportsSinkFailureAndValidatesCallbacks) {
    Fixture f;
    EXPECT_THROW((ReliableChannel{f.limits(), MemoryBudget(100), f.sink, {}, f.callbacks()}),
                 std::invalid_argument);
    EXPECT_THROW((ReliableChannel{f.limits(), MemoryBudget(100), f.sink, f.loop.executor(), {}}),
                 std::invalid_argument);
    auto impossible = f.limits();
    impossible.backend_high_water = frame_header_size;
    EXPECT_THROW(
        (ReliableChannel{impossible, MemoryBudget(100), f.sink, f.loop.executor(), f.callbacks()}),
        std::invalid_argument);
    ReliableChannel channel(f.limits(), MemoryBudget(100), f.sink, f.loop.executor(),
                            f.callbacks());
    channel.set_ready(true);
    f.sink.result = SinkResult::Failed;
    EXPECT_EQ(channel.send({}), ChannelSend::Accepted);
    f.loop.drain();
    ASSERT_EQ(f.errors.size(), 1U);
    EXPECT_EQ(f.errors[0], ChannelFailure::Transport);
    EXPECT_EQ(channel.send({}), ChannelSend::Closed);
}

TEST(ReliableChannel, CallbackCanDestroyChannelDuringDelivery) {
    Fixture f;
    MemoryBudget budget(100);
    std::unique_ptr<ReliableChannel> channel;
    auto callbacks = f.callbacks();
    callbacks.on_message = [&](Bytes) { channel.reset(); };
    channel = std::make_unique<ReliableChannel>(f.limits(), budget, f.sink, f.loop.executor(),
                                                std::move(callbacks));
    channel->set_ready(true);
    auto encoded = std::get<Bytes>(encode_frame(Frame{1, 1, 0, {std::byte{1}}}, {8, 21}));
    channel->on_frame(encoded);
    EXPECT_FALSE(channel);
    EXPECT_EQ(budget.used(), 0U);
    f.loop.drain();
    EXPECT_TRUE(f.errors.empty());
}

TEST(ReliableChannel, SharedBudgetReleaseCanWakeAnotherChannel) {
    Fixture f;
    MemoryBudget budget(23);
    ReliableChannel first(f.limits(), budget, f.sink, f.loop.executor(), f.callbacks());
    ReliableChannel *second_pointer = nullptr;
    ChannelSend retry = ChannelSend::Failed;
    auto callbacks = f.callbacks();
    callbacks.on_writable = [&] { retry = second_pointer->send({}); };
    ReliableChannel second(f.limits(), budget, f.sink, f.loop.executor(), std::move(callbacks));
    second_pointer = &second;
    first.set_ready(true);
    second.set_ready(true);
    f.loop.drain();
    EXPECT_EQ(first.send(Bytes(2)), ChannelSend::Accepted);
    EXPECT_EQ(second.send({}), ChannelSend::Backpressure);
    f.loop.drain();
    EXPECT_EQ(retry, ChannelSend::Failed);
    second.wake();
    f.loop.drain();
    EXPECT_EQ(retry, ChannelSend::Accepted);
    EXPECT_EQ(budget.used(), 0U);
}
