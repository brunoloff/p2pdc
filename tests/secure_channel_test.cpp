#include "../src/security/secure_channel.hpp"
#include "borrowed.hpp"

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
struct Sink final : ReliableSink {
    std::deque<Bytes> frames;
    SinkResult state = SinkResult::Accepted;
    std::size_t buffered_amount() const override { return 0; }
    SinkResult send(ByteView frame) override {
        if (state == SinkResult::Accepted)
            frames.emplace_back(frame.begin(), frame.end());
        return state;
    }
};
struct SignalingPair {
    Identity a = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity b = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    RecipientKeyPair box_a =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    RecipientKeyPair box_b =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    Bytes capability = Bytes(32, std::byte{5});
    SignalingRecipient peer_a{a.public_key(), {std::byte{1}}};
    SignalingRecipient peer_b{b.public_key(), {std::byte{2}}};
    SignalingPolicy policy{100, 60, 5, 4096};
    std::vector<Bytes> a_wire, b_wire;
    std::unique_ptr<AsyncSignaling> left, right;
    SignalingFlowCallbacks callbacks(std::vector<Bytes> &out) {
        return {[&out](Bytes wire) {
                    out.push_back(std::move(wire));
                    return true;
                },
                [](ByteView) { return true; }, [](CandidateView) { return true; },
                [](SignalingKind) { return true; }};
    }
    void create() {
        const AsyncSignalingBinding ab{peer_a,
                                       peer_b,
                                       box_a.public_key(),
                                       box_b.public_key(),
                                       {std::byte{3}},
                                       HandshakeRole::Offerer};
        const AsyncSignalingBinding ba{peer_b,
                                       peer_a,
                                       box_b.public_key(),
                                       box_a.public_key(),
                                       {std::byte{3}},
                                       HandshakeRole::Answerer};
        left = std::make_unique<AsyncSignaling>(ab, capability, CandidateLimits{128, 32, 256},
                                                MemoryBudget(1024), 1024, 8, callbacks(a_wire));
        right = std::make_unique<AsyncSignaling>(ba, capability, CandidateLimits{128, 32, 256},
                                                 MemoryBudget(1024), 1024, 8, callbacks(b_wire));
    }
    void install(std::byte answer = std::byte{2}) {
        ASSERT_EQ(left->local_description(a, capability, Bytes{std::byte{1}}, 100, 150, policy),
                  FlowResult::Accepted);
        ASSERT_EQ(right->receive(a_wire[0], box_b, capability, policy), FlowResult::Accepted);
        ASSERT_EQ(right->description_applied(true), FlowResult::Accepted);
        ASSERT_EQ(right->local_description(b, capability, Bytes{answer}, 100, 150, policy),
                  FlowResult::Accepted);
        ASSERT_EQ(left->receive(b_wire[0], box_a, capability, policy), FlowResult::Accepted);
        ASSERT_EQ(left->description_applied(true), FlowResult::Accepted);
    }
};
struct Pair {
    SignalingPair signals;
    Loop loop;
    Sink a_sink, b_sink;
    std::unique_ptr<SecureChannel> a, b;
    std::vector<Bytes> received;
    std::vector<SecureFailure> errors;
    unsigned ready_count = 0;
    SecureChannelCallbacks callbacks() {
        return {[this](std::uint32_t limit) {
                    EXPECT_EQ(limit, 100U);
                    ++ready_count;
                },
                [this](Bytes message) { received.push_back(std::move(message)); }, [] {},
                [this](SecureFailure reason) { errors.push_back(reason); }};
    }
    void create() {
        signals.create();
        ReliableChannelLimits limits{{201, 64}, 4096, 16, 256, 4};
        a = std::make_unique<SecureChannel>(*signals.left, borrowed(signals.a),
                                            HandshakeRole::Offerer, 100, limits, MemoryBudget(8192),
                                            a_sink, loop.executor(), callbacks());
        b = std::make_unique<SecureChannel>(
            *signals.right, borrowed(signals.b), HandshakeRole::Answerer, 100, limits,
            MemoryBudget(8192), b_sink, loop.executor(), callbacks());
    }
    void transfer() {
        for (int count = 0; count < 100; ++count) {
            loop.drain();
            if (a_sink.frames.empty() && b_sink.frames.empty())
                return;
            while (!a_sink.frames.empty()) {
                auto frame = std::move(a_sink.frames.front());
                a_sink.frames.pop_front();
                b->on_frame(frame);
            }
            while (!b_sink.frames.empty()) {
                auto frame = std::move(b_sink.frames.front());
                b_sink.frames.pop_front();
                a->on_frame(frame);
            }
        }
        ADD_FAILURE() << "transfer did not quiesce";
    }
};
} // namespace

TEST(SecureChannel, AuthenticatesBeforeFragmentedBinaryExchange) {
    Pair pair;
    pair.create();
    EXPECT_FALSE(pair.a->start());
    EXPECT_EQ(pair.a->send({}), ChannelSend::NotReady);
    pair.signals.install();
    ASSERT_TRUE(pair.a->start());
    ASSERT_TRUE(pair.b->start());
    pair.a->set_transport_ready(true);
    pair.b->set_transport_ready(true);
    EXPECT_EQ(pair.a->send({}), ChannelSend::NotReady);
    pair.transfer();
    ASSERT_TRUE(pair.a->ready());
    ASSERT_TRUE(pair.b->ready());
    EXPECT_EQ(pair.ready_count, 2U);
    const Bytes message(90, std::byte{255});
    EXPECT_EQ(pair.a->send(message), ChannelSend::Accepted);
    pair.transfer();
    ASSERT_EQ(pair.received.size(), 1U);
    EXPECT_EQ(pair.received.front(), message);
    EXPECT_TRUE(pair.errors.empty());
    EXPECT_EQ(pair.a->send(Bytes(101)), ChannelSend::TooLarge);
}

TEST(SecureChannel, PrematureFrameAndTimeoutFailClosed) {
    Pair first;
    first.create();
    first.a->on_frame(Bytes{std::byte{1}});
    ASSERT_EQ(first.errors.size(), 1U);
    EXPECT_EQ(first.errors.front(), SecureFailure::Protocol);
    EXPECT_EQ(first.a->send({}), ChannelSend::Closed);
    Pair second;
    second.create();
    second.a->timeout();
    ASSERT_EQ(second.errors.size(), 1U);
    EXPECT_EQ(second.errors.front(), SecureFailure::Timeout);
    EXPECT_FALSE(second.a->start());
}

TEST(SecureChannel, SuspendAndRecoverWithoutPrematureDelivery) {
    Pair pair;
    pair.create();
    pair.signals.install();
    ASSERT_TRUE(pair.a->start());
    ASSERT_TRUE(pair.b->start());
    pair.a->set_transport_ready(true);
    pair.b->set_transport_ready(true);
    pair.transfer();
    ASSERT_TRUE(pair.a->ready());
    pair.a->set_transport_ready(false);
    EXPECT_FALSE(pair.a->ready());
    EXPECT_EQ(pair.a->send({}), ChannelSend::NotReady);
    pair.a->set_transport_ready(true);
    EXPECT_TRUE(pair.a->ready());
    EXPECT_EQ(pair.a->send(Bytes{std::byte{0}}), ChannelSend::Accepted);
    pair.transfer();
    ASSERT_EQ(pair.received.size(), 1U);
    EXPECT_EQ(pair.received[0], Bytes{std::byte{0}});
}

TEST(SecureChannel, RejectsIdentityAndInvalidConfiguration) {
    Pair pair;
    pair.create();
    pair.signals.install();
    ReliableChannelLimits limits{{201, 64}, 4096, 16, 256, 4};
    EXPECT_THROW((SecureChannel{*pair.signals.left, borrowed(pair.signals.a),
                                HandshakeRole::Offerer, 201, limits, MemoryBudget(8192),
                                pair.a_sink, pair.loop.executor(), pair.callbacks()}),
                 std::invalid_argument);
    EXPECT_THROW((SecureChannel{*pair.signals.left, borrowed(pair.signals.a),
                                HandshakeRole::Offerer, 0, limits, MemoryBudget(8192), pair.a_sink,
                                pair.loop.executor(), pair.callbacks()}),
                 std::invalid_argument);
    SecureChannel wrong(*pair.signals.left, borrowed(pair.signals.b), HandshakeRole::Offerer, 100,
                        limits, MemoryBudget(8192), pair.a_sink, pair.loop.executor(),
                        pair.callbacks());
    EXPECT_FALSE(wrong.start());
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors[0], SecureFailure::Protocol);
}

TEST(SecureChannel, RejectsPeerWithDifferentVerifiedContext) {
    Pair first, second;
    first.create();
    second.create();
    first.signals.install();
    second.signals.install(std::byte{7});
    ASSERT_TRUE(first.a->start());
    ASSERT_TRUE(second.b->start());
    first.a->set_transport_ready(true);
    second.b->set_transport_ready(true);
    for (int count = 0; count < 30 && first.errors.empty() && second.errors.empty(); ++count) {
        first.loop.drain();
        second.loop.drain();
        while (!first.a_sink.frames.empty()) {
            auto frame = std::move(first.a_sink.frames.front());
            first.a_sink.frames.pop_front();
            second.b->on_frame(frame);
        }
        while (!second.b_sink.frames.empty()) {
            auto frame = std::move(second.b_sink.frames.front());
            second.b_sink.frames.pop_front();
            first.a->on_frame(frame);
        }
    }
    EXPECT_TRUE(!first.errors.empty() || !second.errors.empty());
    EXPECT_FALSE(first.a->ready());
    EXPECT_FALSE(second.b->ready());
    EXPECT_TRUE(first.received.empty());
    EXPECT_TRUE(second.received.empty());
}

TEST(SecureChannel, RetriesHandshakeAfterBackendBackpressure) {
    Pair pair;
    pair.create();
    pair.signals.install();
    ASSERT_TRUE(pair.a->start());
    ASSERT_TRUE(pair.b->start());
    pair.a_sink.state = SinkResult::NotReady;
    pair.a->set_transport_ready(true);
    pair.b->set_transport_ready(true);
    pair.transfer();
    EXPECT_FALSE(pair.a->ready());
    EXPECT_FALSE(pair.b->ready());
    pair.a_sink.state = SinkResult::Accepted;
    pair.a->wake();
    pair.transfer();
    EXPECT_TRUE(pair.a->ready());
    EXPECT_TRUE(pair.b->ready());
    EXPECT_TRUE(pair.errors.empty());
}

TEST(SecureChannel, TimeoutDuringHandshakeSuppressesPendingWork) {
    Pair pair;
    pair.create();
    pair.signals.install();
    ASSERT_TRUE(pair.a->start());
    pair.a->set_transport_ready(true);
    pair.a->timeout();
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors.front(), SecureFailure::Timeout);
    pair.loop.drain();
    EXPECT_TRUE(pair.a_sink.frames.empty());
    EXPECT_EQ(pair.a->send({}), ChannelSend::Closed);
}

TEST(SecureChannel, TransportMayBeReadyBeforeAuthenticationStarts) {
    Pair pair;
    pair.create();
    pair.a->set_transport_ready(true);
    pair.b->set_transport_ready(true);
    pair.signals.install();
    ASSERT_TRUE(pair.a->start());
    ASSERT_TRUE(pair.b->start());
    pair.transfer();
    EXPECT_TRUE(pair.a->ready());
    EXPECT_TRUE(pair.b->ready());
}

TEST(SecureChannel, MalformedFramesFailOnceAndCloseSuppressesQueuedWork) {
    Pair pair;
    pair.create();
    pair.signals.install();
    ASSERT_TRUE(pair.a->start());
    pair.a->set_transport_ready(true);
    pair.a->on_frame(Bytes{std::byte{0}});
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors.front(), SecureFailure::Protocol);
    pair.a->on_frame(Bytes{std::byte{0}});
    pair.a->timeout();
    pair.loop.drain();
    EXPECT_EQ(pair.errors.size(), 1U);
    EXPECT_TRUE(pair.a_sink.frames.empty());
    pair.b->close();
    pair.loop.drain();
    EXPECT_TRUE(pair.b_sink.frames.empty());
}
