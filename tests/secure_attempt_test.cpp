#include "../src/security/secure_attempt.hpp"
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
    std::size_t buffered_amount() const override { return 0; }
    SinkResult send(ByteView frame) override {
        frames.emplace_back(frame.begin(), frame.end());
        return SinkResult::Accepted;
    }
};
struct Pair {
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
    std::vector<Bytes> a_wire, b_wire, received;
    std::vector<AttemptFailure> errors;
    unsigned ready = 0, descriptions = 0, candidates = 0;
    bool publish = true;
    Loop loop;
    Sink sink_a, sink_b;
    std::unique_ptr<SecureAttempt> left, right;
    AsyncSignalingBinding binding(bool first) {
        return {first ? peer_a : peer_b,
                first ? peer_b : peer_a,
                first ? box_a.public_key() : box_b.public_key(),
                first ? box_b.public_key() : box_a.public_key(),
                {std::byte{3}},
                first ? HandshakeRole::Offerer : HandshakeRole::Answerer};
    }
    SecureAttemptCallbacks callbacks(std::vector<Bytes> &out) {
        SignalingFlowCallbacks signaling{[this, &out](Bytes wire) {
                                             out.push_back(std::move(wire));
                                             return publish;
                                         },
                                         [this](ByteView) {
                                             ++descriptions;
                                             return true;
                                         },
                                         [this](CandidateView) {
                                             ++candidates;
                                             return true;
                                         },
                                         [](SignalingKind) { return true; }};
        return {std::move(signaling),
                [this](std::uint32_t limit) {
                    EXPECT_EQ(limit, 100U);
                    ++ready;
                },
                [this](Bytes value) { received.push_back(std::move(value)); }, [] {},
                [this](AttemptFailure reason) { errors.push_back(reason); }};
    }
    std::unique_ptr<SecureAttempt> make(bool first) {
        const ReliableChannelLimits limits{{201, 64}, 4096, 16, 256, 4};
        return std::make_unique<SecureAttempt>(
            binding(first), capability, borrowed(first ? a : b), first ? box_a : box_b,
            CandidateLimits{128, 32, 256}, MemoryBudget(8192), limits, 100, first ? sink_a : sink_b,
            loop.executor(), callbacks(first ? a_wire : b_wire));
    }
    void transfer() {
        for (int count = 0; count < 100; ++count) {
            loop.drain();
            if (sink_a.frames.empty() && sink_b.frames.empty())
                return;
            while (!sink_a.frames.empty()) {
                auto value = std::move(sink_a.frames.front());
                sink_a.frames.pop_front();
                right->on_frame(value);
            }
            while (!sink_b.frames.empty()) {
                auto value = std::move(sink_b.frames.front());
                sink_b.frames.pop_front();
                left->on_frame(value);
            }
        }
        ADD_FAILURE() << "transport did not quiesce";
    }
};
} // namespace

TEST(SecureAttempt, VerifiedSignalingAutomaticallyStartsAuthenticatedChannel) {
    Pair pair;
    pair.left = pair.make(true);
    pair.right = pair.make(false);
    pair.left->set_transport_ready(true);
    pair.right->set_transport_ready(true);
    ASSERT_EQ(pair.left->local_description(Bytes{std::byte{1}}, 100, 150, pair.policy),
              FlowResult::Accepted);
    ASSERT_EQ(pair.right->receive_signaling(pair.a_wire[0], pair.policy), FlowResult::Accepted);
    ASSERT_EQ(pair.right->local_description(Bytes{std::byte{2}}, 100, 150, pair.policy),
              FlowResult::Accepted);
    ASSERT_EQ(pair.left->receive_signaling(pair.b_wire[0], pair.policy), FlowResult::Accepted);
    EXPECT_EQ(pair.left->send({}), ChannelSend::NotReady);
    ASSERT_EQ(pair.left->description_applied(true), FlowResult::Accepted);
    ASSERT_EQ(pair.right->description_applied(true), FlowResult::Accepted);
    pair.transfer();
    ASSERT_TRUE(pair.left->ready());
    ASSERT_TRUE(pair.right->ready());
    EXPECT_EQ(pair.ready, 2U);
    const Bytes value(90, std::byte{255});
    EXPECT_EQ(pair.left->send(value), ChannelSend::Accepted);
    pair.transfer();
    ASSERT_EQ(pair.received.size(), 1U);
    EXPECT_EQ(pair.received.front(), value);
    EXPECT_TRUE(pair.errors.empty());
}

TEST(SecureAttempt, SignalingFailureClosesChannelAndLateEvents) {
    Pair pair;
    pair.left = pair.make(true);
    pair.publish = false;
    EXPECT_EQ(pair.left->local_description(Bytes{std::byte{1}}, 100, 150, pair.policy),
              FlowResult::Failed);
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors[0], AttemptFailure::Signaling);
    EXPECT_EQ(pair.left->description_applied(true), FlowResult::Closed);
    EXPECT_EQ(pair.left->send({}), ChannelSend::Closed);
    pair.left->timeout();
    EXPECT_EQ(pair.errors.size(), 1U);
}

TEST(SecureAttempt, InvalidIdentityAndCapabilityAreRejected) {
    Pair pair;
    const ReliableChannelLimits limits{{201, 64}, 4096, 16, 256, 4};
    EXPECT_THROW((SecureAttempt{pair.binding(true),
                                pair.capability,
                                borrowed(pair.b),
                                pair.box_a,
                                {128, 32, 256},
                                MemoryBudget(8192),
                                limits,
                                100,
                                pair.sink_a,
                                pair.loop.executor(),
                                pair.callbacks(pair.a_wire)}),
                 std::invalid_argument);
    EXPECT_THROW((SecureAttempt{pair.binding(true),
                                Bytes(31),
                                borrowed(pair.a),
                                pair.box_a,
                                {128, 32, 256},
                                MemoryBudget(8192),
                                limits,
                                100,
                                pair.sink_a,
                                pair.loop.executor(),
                                pair.callbacks(pair.a_wire)}),
                 std::invalid_argument);
    EXPECT_THROW((SecureAttempt{pair.binding(true),
                                pair.capability,
                                borrowed(pair.a),
                                pair.box_b,
                                {128, 32, 256},
                                MemoryBudget(8192),
                                limits,
                                100,
                                pair.sink_a,
                                pair.loop.executor(),
                                pair.callbacks(pair.a_wire)}),
                 std::invalid_argument);
}

TEST(SecureAttempt, TimeoutAndPrematureFramesFailClosed) {
    Pair first;
    first.left = first.make(true);
    first.left->on_frame(Bytes{std::byte{0}});
    ASSERT_EQ(first.errors.size(), 1U);
    EXPECT_EQ(first.errors[0], AttemptFailure::Protocol);
    Pair second;
    second.left = second.make(true);
    second.left->timeout();
    ASSERT_EQ(second.errors.size(), 1U);
    EXPECT_EQ(second.errors[0], AttemptFailure::Timeout);
    EXPECT_FALSE(second.left->ready());
}

TEST(SecureAttempt, AuthenticatedCancelTerminatesAttempt) {
    Pair pair;
    pair.left = pair.make(true);
    pair.right = pair.make(false);
    ASSERT_EQ(pair.left->local_description(Bytes{std::byte{1}}, 100, 150, pair.policy),
              FlowResult::Accepted);
    ASSERT_EQ(pair.right->receive_signaling(pair.a_wire[0], pair.policy), FlowResult::Accepted);
    SignalingFields fields{
        SignalingKind::Cancel, pair.peer_a.instance, pair.peer_b, {std::byte{3}}, 1, 100, 150, {}};
    auto encoded =
        encode_signaling(pair.a, fields, pair.box_b.public_key(), pair.capability, pair.policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    EXPECT_EQ(pair.right->receive_signaling(std::get<Bytes>(encoded), pair.policy),
              FlowResult::Failed);
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors.front(), AttemptFailure::Canceled);
    EXPECT_EQ(pair.right->send({}), ChannelSend::Closed);
    EXPECT_EQ(pair.right->description_applied(true), FlowResult::Closed);
}

TEST(SecureAttempt, AuthenticatedRejectionIsReportedAndTamperingIsIgnored) {
    Pair pair;
    pair.left = pair.make(true);
    ASSERT_EQ(pair.left->local_description(Bytes{std::byte{1}}, 100, 150, pair.policy),
              FlowResult::Accepted);
    SignalingFields fields{
        SignalingKind::Reject, pair.peer_b.instance, pair.peer_a, {std::byte{3}}, 0, 100, 150, {}};
    auto encoded =
        encode_signaling(pair.b, fields, pair.box_a.public_key(), pair.capability, pair.policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    auto wire = std::get<Bytes>(encoded);
    auto tampered = wire;
    tampered.back() ^= std::byte{1};
    EXPECT_EQ(pair.left->receive_signaling(tampered, pair.policy), FlowResult::Ignored);
    EXPECT_TRUE(pair.errors.empty());
    EXPECT_EQ(pair.left->receive_signaling(wire, pair.policy), FlowResult::Failed);
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors.front(), AttemptFailure::Rejected);
}

TEST(SecureAttempt, InvalidCandidateCompletionStopsAttempt) {
    Pair pair;
    pair.left = pair.make(true);
    pair.right = pair.make(false);
    ASSERT_EQ(pair.left->local_description(Bytes{std::byte{1}}, 100, 150, pair.policy),
              FlowResult::Accepted);
    ASSERT_EQ(pair.left->local_candidate({"candidate", "0"}, 100, 150, pair.policy),
              FlowResult::Accepted);
    ASSERT_EQ(pair.right->receive_signaling(pair.a_wire[1], pair.policy), FlowResult::Accepted);
    ASSERT_EQ(pair.right->receive_signaling(pair.a_wire[0], pair.policy), FlowResult::Accepted);
    EXPECT_EQ(pair.candidates, 0U);
    ASSERT_EQ(pair.right->description_applied(true), FlowResult::Accepted);
    EXPECT_EQ(pair.candidates, 1U);
    ASSERT_TRUE(pair.right->candidate_operation());
    EXPECT_EQ(pair.right->candidate_applied(*pair.right->candidate_operation() + 1, true),
              FlowResult::Failed);
    EXPECT_EQ(pair.right->send({}), ChannelSend::Closed);
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors[0], AttemptFailure::Signaling);
}

TEST(SecureAttempt, RemoteDescriptionFailureStopsAttempt) {
    Pair pair;
    pair.left = pair.make(true);
    pair.right = pair.make(false);
    ASSERT_EQ(pair.left->local_description(Bytes{std::byte{1}}, 100, 150, pair.policy),
              FlowResult::Accepted);
    ASSERT_EQ(pair.right->receive_signaling(pair.a_wire[0], pair.policy), FlowResult::Accepted);
    EXPECT_EQ(pair.right->description_applied(false), FlowResult::Failed);
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors[0], AttemptFailure::Signaling);
    EXPECT_EQ(pair.right->description_applied(true), FlowResult::Closed);
    EXPECT_FALSE(pair.right->ready());
}
