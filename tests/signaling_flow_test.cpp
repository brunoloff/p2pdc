#include "../src/security/signaling_flow.hpp"
#include "../src/security/signaling_limits.hpp"

#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Fixture {
    Identity alice = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity bob = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    RecipientKeyPair alice_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    RecipientKeyPair bob_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    Bytes capability = Bytes(32, std::byte{5});
    SignalingRecipient peer_a{alice.public_key(), {std::byte{1}}};
    SignalingRecipient peer_b{bob.public_key(), {std::byte{2}}};
    AttemptId attempt{std::byte{3}};
    SignalingPolicy policy{100, 60, 5, 4096};
    SignalingSession left{peer_a,
                          peer_b,
                          alice_box.public_key(),
                          bob_box.public_key(),
                          attempt,
                          HandshakeRole::Offerer,
                          capability,
                          63};
    SignalingSession right{peer_b,
                           peer_a,
                           bob_box.public_key(),
                           alice_box.public_key(),
                           attempt,
                           HandshakeRole::Answerer,
                           capability,
                           63};
    std::vector<Bytes> outgoing;
    std::vector<int> applied;
    bool accept_submission = true;
    bool accept_candidate = true;
    SignalingFlowCallbacks callbacks() {
        return {[this](Bytes wire) {
                    outgoing.push_back(std::move(wire));
                    return accept_submission;
                },
                [this](ByteView) {
                    applied.push_back(1);
                    return true;
                },
                [this](CandidateView) {
                    applied.push_back(2);
                    return accept_candidate;
                },
                [this](SignalingKind) {
                    applied.push_back(3);
                    return true;
                }};
    }
    SignalingFlow make(SignalingSession &session) {
        return SignalingFlow(session, {128, 32, 256}, MemoryBudget(1024), 1024, 8, callbacks());
    }
    FlowResult send(SignalingFlow &flow, SignalingKind kind, ByteView payload) {
        return flow.send(alice, bob_box.public_key(), capability, kind, payload, 100, 150, policy);
    }
};
} // namespace

TEST(SignalingFlow, BuffersCandidatesAndIgnoresReplays) {
    Fixture f;
    auto sender = f.make(f.left);
    auto receiver = f.make(f.right);
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    auto candidate = std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[1], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    EXPECT_TRUE(f.applied.empty());
    ASSERT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    EXPECT_TRUE(receiver.description_installed());
    EXPECT_EQ(f.applied, (std::vector<int>{1, 2}));
    EXPECT_EQ(receiver.receive(f.outgoing[1], f.bob_box, f.capability, f.policy),
              FlowResult::Ignored);
    auto altered = f.outgoing[0];
    altered.back() ^= std::byte{1};
    EXPECT_EQ(receiver.receive(altered, f.bob_box, f.capability, f.policy), FlowResult::Ignored);
    EXPECT_FALSE(receiver.failed());
    receiver.close();
    EXPECT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Closed);
}

TEST(SignalingFlow, FailsWhenEncodedRecordCannotBeSubmitted) {
    Fixture f;
    auto sender = f.make(f.left);
    f.accept_submission = false;
    EXPECT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Failed);
    EXPECT_TRUE(sender.failed());
    EXPECT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Failed);
}

TEST(SignalingFlow, FailsWhenVerifiedCandidateCannotBeApplied) {
    Fixture f;
    auto sender = f.make(f.left);
    auto receiver = f.make(f.right);
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    auto candidate = std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    f.accept_candidate = false;
    EXPECT_EQ(receiver.receive(f.outgoing[1], f.bob_box, f.capability, f.policy),
              FlowResult::Failed);
    EXPECT_TRUE(receiver.failed());
}

TEST(SignalingFlow, LimitsRecordsToReplayWindow) {
    Fixture f;
    auto sender = f.make(f.left);
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    for (int index = 0; index < 62; ++index) {
        ASSERT_EQ(f.send(sender, SignalingKind::Candidate, Bytes{std::byte{9}}),
                  FlowResult::Accepted);
    }
    EXPECT_EQ(f.send(sender, SignalingKind::Candidate, Bytes{std::byte{9}}), FlowResult::Failed);
    EXPECT_EQ(f.outgoing.size(), 63U);
}

TEST(SignalingFlow, AcceptsMaximumDescriptionAndRejectsLargerDescription) {
    Fixture accepted;
    accepted.policy.max_payload_size = signaling_payload_limit;
    auto sender = accepted.make(accepted.left);
    auto receiver = accepted.make(accepted.right);
    Bytes description(signaling_payload_limit, std::byte{'s'});
    ASSERT_EQ(accepted.send(sender, SignalingKind::Offer, description), FlowResult::Accepted);
    ASSERT_EQ(accepted.outgoing.front().size(), signaling_wire_limit);
    EXPECT_EQ(receiver.receive(accepted.outgoing.front(), accepted.bob_box, accepted.capability,
                               accepted.policy),
              FlowResult::Accepted);
    EXPECT_EQ(accepted.applied, (std::vector<int>{1}));

    Fixture oversized;
    oversized.policy.max_payload_size = signaling_payload_limit;
    auto rejected_sender = oversized.make(oversized.left);
    description.push_back(std::byte{'s'});
    EXPECT_EQ(oversized.send(rejected_sender, SignalingKind::Offer, description),
              FlowResult::Failed);
    EXPECT_TRUE(oversized.outgoing.empty());
}

TEST(SignalingFlow, AcceptsCompleteWindowInReverseOrder) {
    Fixture f;
    auto sender = f.make(f.left);
    SignalingFlow receiver(f.right, {128, 32, 256}, MemoryBudget(8192), 8192, 63, f.callbacks());
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    auto candidate = std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    for (int index = 0; index < 62; ++index) {
        ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    }
    for (std::size_t index = f.outgoing.size(); index > 0; --index) {
        ASSERT_EQ(receiver.receive(f.outgoing[index - 1], f.bob_box, f.capability, f.policy),
                  FlowResult::Accepted);
    }
    ASSERT_EQ(f.applied.size(), 63U);
    EXPECT_EQ(f.applied.front(), 1);
    EXPECT_EQ(f.applied.back(), 2);
}

TEST(SignalingFlow, FailsForUnusableLocalRecipientKey) {
    Fixture f;
    auto sender = f.make(f.left);
    auto receiver = f.make(f.right);
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    auto key_owner = std::move(f.bob_box);
    EXPECT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Failed);
    EXPECT_TRUE(receiver.failed());
    EXPECT_EQ(key_owner.public_key(), f.bob_box.public_key());
}

TEST(SignalingFlow, AsyncApplicationWaitsForDescriptionAndEachCandidate) {
    Fixture f;
    auto sender = f.make(f.left);
    auto handlers = f.callbacks();
    handlers.asynchronous_application = true;
    SignalingFlow receiver(f.right, {128, 32, 256}, MemoryBudget(1024), 1024, 8, handlers);
    const auto candidate =
        std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[1], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[2], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    EXPECT_FALSE(receiver.description_installed());
    EXPECT_EQ(f.applied, (std::vector<int>{1}));
    ASSERT_EQ(receiver.description_applied(true), FlowResult::Accepted);
    EXPECT_TRUE(receiver.description_installed());
    EXPECT_EQ(f.applied, (std::vector<int>{1, 2}));
    const auto first = *receiver.candidate_operation();
    ASSERT_EQ(receiver.candidate_applied(first, true), FlowResult::Accepted);
    EXPECT_EQ(f.applied, (std::vector<int>{1, 2, 2}));
    const auto second = *receiver.candidate_operation();
    EXPECT_NE(first, second);
    ASSERT_EQ(receiver.candidate_applied(second, true), FlowResult::Accepted);
    EXPECT_EQ(receiver.candidate_applied(second, true), FlowResult::Failed);
    EXPECT_FALSE(receiver.description_installed());
}

TEST(SignalingFlow, AsyncApplicationFailureAndLateCompletionFailClosed) {
    Fixture f;
    auto sender = f.make(f.left);
    auto handlers = f.callbacks();
    handlers.asynchronous_application = true;
    SignalingFlow receiver(f.right, {128, 32, 256}, MemoryBudget(1024), 1024, 8, handlers);
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    EXPECT_EQ(receiver.description_applied(false), FlowResult::Failed);
    EXPECT_FALSE(receiver.description_installed());
    EXPECT_EQ(receiver.description_applied(true), FlowResult::Failed);
    receiver.close();
    EXPECT_EQ(receiver.description_applied(true), FlowResult::Closed);
}

TEST(SignalingFlow, AsyncCandidateFailureStopsAttempt) {
    Fixture f;
    auto sender = f.make(f.left);
    auto handlers = f.callbacks();
    handlers.asynchronous_application = true;
    SignalingFlow receiver(f.right, {128, 32, 256}, MemoryBudget(1024), 1024, 8, handlers);
    const auto candidate =
        std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[0], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(receiver.description_applied(true), FlowResult::Accepted);
    ASSERT_EQ(receiver.receive(f.outgoing[1], f.bob_box, f.capability, f.policy),
              FlowResult::Accepted);
    EXPECT_EQ(receiver.candidate_applied(*receiver.candidate_operation(), false),
              FlowResult::Failed);
    EXPECT_FALSE(receiver.description_installed());
}

TEST(SignalingFlow, StaleCandidateCompletionCannotFinishNextCandidate) {
    Fixture f;
    auto sender = f.make(f.left);
    auto handlers = f.callbacks();
    handlers.asynchronous_application = true;
    SignalingFlow receiver(f.right, {128, 32, 256}, MemoryBudget(1024), 1024, 8, handlers);
    const auto candidate =
        std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    for (const auto &wire : f.outgoing)
        ASSERT_EQ(receiver.receive(wire, f.bob_box, f.capability, f.policy), FlowResult::Accepted);
    ASSERT_EQ(receiver.description_applied(true), FlowResult::Accepted);
    const auto first = *receiver.candidate_operation();
    ASSERT_EQ(receiver.candidate_applied(first, true), FlowResult::Accepted);
    ASSERT_NE(receiver.candidate_operation(), first);
    EXPECT_EQ(receiver.candidate_applied(first, true), FlowResult::Failed);
    EXPECT_FALSE(receiver.candidate_operation());
}

TEST(SignalingFlow, ClosingDuringAsyncCandidateSuppressesCompletion) {
    Fixture f;
    auto sender = f.make(f.left);
    auto handlers = f.callbacks();
    handlers.asynchronous_application = true;
    SignalingFlow receiver(f.right, {128, 32, 256}, MemoryBudget(1024), 1024, 8, handlers);
    const auto candidate =
        std::get<Bytes>(encode_candidate({"candidate", "audio"}, {128, 32, 256}));
    ASSERT_EQ(f.send(sender, SignalingKind::Offer, Bytes{std::byte{1}}), FlowResult::Accepted);
    ASSERT_EQ(f.send(sender, SignalingKind::Candidate, candidate), FlowResult::Accepted);
    for (const auto &wire : f.outgoing)
        ASSERT_EQ(receiver.receive(wire, f.bob_box, f.capability, f.policy), FlowResult::Accepted);
    ASSERT_EQ(receiver.description_applied(true), FlowResult::Accepted);
    const auto operation = *receiver.candidate_operation();
    receiver.close();
    EXPECT_FALSE(receiver.description_installed());
    EXPECT_FALSE(receiver.candidate_operation());
    EXPECT_EQ(receiver.candidate_applied(operation, true), FlowResult::Closed);
}
