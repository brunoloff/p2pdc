#include "../src/security/async_signaling.hpp"

#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Fixture {
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
    std::vector<Bytes> from_a, from_b;
    unsigned remote_descriptions = 0, remote_candidates = 0;
    bool allow_submit = true;
    SignalingFlowCallbacks callbacks(std::vector<Bytes> &out) {
        return {[this, &out](Bytes wire) {
                    out.push_back(std::move(wire));
                    return allow_submit;
                },
                [this](ByteView) {
                    ++remote_descriptions;
                    return true;
                },
                [this](CandidateView) {
                    ++remote_candidates;
                    return true;
                },
                [](SignalingKind) { return true; }};
    }
    std::unique_ptr<AsyncSignaling> make(bool first) {
        AsyncSignalingBinding binding{first ? peer_a : peer_b,
                                      first ? peer_b : peer_a,
                                      first ? box_a.public_key() : box_b.public_key(),
                                      first ? box_b.public_key() : box_a.public_key(),
                                      {std::byte{3}},
                                      first ? HandshakeRole::Offerer : HandshakeRole::Answerer};
        return std::make_unique<AsyncSignaling>(binding, capability, CandidateLimits{128, 32, 256},
                                                MemoryBudget(1024), 1024, 8,
                                                callbacks(first ? from_a : from_b));
    }
};
} // namespace

TEST(AsyncSignaling, ContextRequiresBothInstalledDescriptions) {
    Fixture f;
    auto a = f.make(true), b = f.make(false);
    EXPECT_FALSE(a->handshake_context());
    ASSERT_EQ(a->local_description(f.a, f.capability, Bytes{std::byte{1}}, 100, 150, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(b->receive(f.from_a[0], f.box_b, f.capability, f.policy), FlowResult::Accepted);
    ASSERT_EQ(b->local_description(f.b, f.capability, Bytes{std::byte{2}}, 100, 150, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(a->receive(f.from_b[0], f.box_a, f.capability, f.policy), FlowResult::Accepted);
    EXPECT_FALSE(a->handshake_context());
    EXPECT_FALSE(b->handshake_context());
    ASSERT_EQ(a->description_applied(true), FlowResult::Accepted);
    ASSERT_EQ(b->description_applied(true), FlowResult::Accepted);
    const auto left = a->handshake_context(), right = b->handshake_context();
    ASSERT_TRUE(left && right);
    EXPECT_EQ(left->offer_hash, right->offer_hash);
    EXPECT_EQ(left->answer_hash, right->answer_hash);
    EXPECT_EQ(left->group, right->group);
    EXPECT_EQ(f.remote_descriptions, 2U);
    b->close();
    EXPECT_FALSE(b->handshake_context());
}

TEST(AsyncSignaling, FailedInstallAndLateCompletionCannotProduceContext) {
    Fixture f;
    auto a = f.make(true), b = f.make(false);
    ASSERT_EQ(a->local_description(f.a, f.capability, Bytes{std::byte{1}}, 100, 150, f.policy),
              FlowResult::Accepted);
    auto corrupt = f.from_a[0];
    corrupt.back() ^= std::byte{1};
    EXPECT_EQ(b->receive(corrupt, f.box_b, f.capability, f.policy), FlowResult::Ignored);
    EXPECT_EQ(f.remote_descriptions, 0U);
    ASSERT_EQ(b->receive(f.from_a[0], f.box_b, f.capability, f.policy), FlowResult::Accepted);
    ASSERT_EQ(b->description_applied(false), FlowResult::Failed);
    EXPECT_FALSE(b->handshake_context());
    EXPECT_EQ(b->description_applied(true), FlowResult::Failed);
    b->close();
    EXPECT_EQ(b->description_applied(true), FlowResult::Closed);
}

TEST(AsyncSignaling, LocalOrderAndSubmissionFailuresAreSticky) {
    Fixture f;
    auto a = f.make(true);
    EXPECT_EQ(a->local_candidate(f.a, f.capability, {"candidate", "0"}, 100, 150, f.policy),
              FlowResult::Failed);
    EXPECT_EQ(a->local_description(f.a, f.capability, Bytes{std::byte{1}}, 100, 150, f.policy),
              FlowResult::Failed);
    auto other = f.make(true);
    f.allow_submit = false;
    EXPECT_EQ(other->local_description(f.a, f.capability, Bytes{std::byte{1}}, 100, 150, f.policy),
              FlowResult::Failed);
    EXPECT_FALSE(other->handshake_context());
}

TEST(AsyncSignaling, InstalledRemoteAloneDoesNotAuthorizeHandshake) {
    Fixture f;
    auto a = f.make(true), b = f.make(false);
    ASSERT_EQ(a->local_description(f.a, f.capability, Bytes{std::byte{1}}, 100, 150, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(b->receive(f.from_a[0], f.box_b, f.capability, f.policy), FlowResult::Accepted);
    ASSERT_EQ(b->description_applied(true), FlowResult::Accepted);
    EXPECT_FALSE(b->handshake_context());
    ASSERT_EQ(b->local_description(f.b, f.capability, Bytes{std::byte{2}}, 100, 150, f.policy),
              FlowResult::Accepted);
    EXPECT_TRUE(b->handshake_context());
    EXPECT_EQ(b->local_description(f.b, f.capability, Bytes{std::byte{2}}, 100, 150, f.policy),
              FlowResult::Failed);
    EXPECT_FALSE(b->handshake_context());
}

TEST(AsyncSignaling, CandidatesWaitForRemoteInstallAndCompletion) {
    Fixture f;
    auto a = f.make(true), b = f.make(false);
    ASSERT_EQ(a->local_description(f.a, f.capability, Bytes{std::byte{1}}, 100, 150, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(a->local_candidate(f.a, f.capability, {"candidate", "0"}, 100, 150, f.policy),
              FlowResult::Accepted);
    ASSERT_EQ(b->receive(f.from_a[1], f.box_b, f.capability, f.policy), FlowResult::Accepted);
    ASSERT_EQ(b->receive(f.from_a[0], f.box_b, f.capability, f.policy), FlowResult::Accepted);
    EXPECT_EQ(f.remote_candidates, 0U);
    ASSERT_EQ(b->description_applied(true), FlowResult::Accepted);
    EXPECT_EQ(f.remote_candidates, 1U);
    const auto operation = b->candidate_operation();
    ASSERT_TRUE(operation);
    EXPECT_EQ(b->candidate_applied(*operation, true), FlowResult::Accepted);
    EXPECT_EQ(b->candidate_applied(*operation, true), FlowResult::Failed);
}
