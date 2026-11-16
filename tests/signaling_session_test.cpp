#include "../src/security/signaling_session.hpp"
#include "borrowed.hpp"
#include "p2pdc/handshake_exchange.h"

#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Fixture {
    Identity a = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity b = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    Identity other = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{3})));
    RecipientKeyPair box_a =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    RecipientKeyPair box_b =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{5})));
    Bytes capability = Bytes(32, std::byte{6});
    SignalingRecipient peer_a{a.public_key(), {std::byte{1}}};
    SignalingRecipient peer_b{b.public_key(), {std::byte{2}}};
    AttemptId attempt{std::byte{3}};
    SignalingPolicy policy{100, 60, 5, 4096};
    SignalingSession left{
        peer_a,     peer_b, box_a.public_key(), box_b.public_key(), attempt, HandshakeRole::Offerer,
        capability, 8};
    SignalingSession right{peer_b,
                           peer_a,
                           box_b.public_key(),
                           box_a.public_key(),
                           attempt,
                           HandshakeRole::Answerer,
                           capability,
                           8};
    Bytes encode(SignalingSession &session, const Identity &id, const RecipientBoxPublicKey &box,
                 SignalingKind kind, ByteView payload) {
        return std::get<Bytes>(
            session.encode(id, box, capability, kind, payload, 100, 150, policy));
    }
};
} // namespace

TEST(SignalingSession, VerifiesOutOfOrderCandidateAndBuildsSameHandshakeContext) {
    Fixture f;
    const Bytes offer{std::byte{1}, std::byte{2}};
    const Bytes answer{std::byte{3}, std::byte{4}};
    const auto signed_offer =
        f.encode(f.left, f.a, f.box_b.public_key(), SignalingKind::Offer, offer);
    const auto candidate =
        f.encode(f.left, f.a, f.box_b.public_key(), SignalingKind::Candidate, Bytes{std::byte{9}});
    const auto signed_answer =
        f.encode(f.right, f.b, f.box_a.public_key(), SignalingKind::Answer, answer);
    EXPECT_EQ(std::get<SessionError>(f.left.handshake_context()), SessionError::MissingDescription);
    auto accepted_candidate = f.right.verify(candidate, f.box_b, f.capability, f.policy);
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(accepted_candidate));
    EXPECT_EQ(std::get<SignalingEnvelope>(accepted_candidate).fields.kind,
              SignalingKind::Candidate);
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(
        f.right.verify(signed_offer, f.box_b, f.capability, f.policy)));
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(
        f.left.verify(signed_answer, f.box_a, f.capability, f.policy)));
    const auto a = std::get<HandshakeContext>(f.left.handshake_context());
    const auto b = std::get<HandshakeContext>(f.right.handshake_context());
    EXPECT_EQ(a.offerer_key, b.offerer_key);
    EXPECT_EQ(a.answerer_key, b.answerer_key);
    EXPECT_EQ(a.offer_hash, b.offer_hash);
    EXPECT_EQ(a.answer_hash, b.answer_hash);
    EXPECT_EQ(a.group, b.group);
    HandshakeExchange first(borrowed(f.a), a, HandshakeRole::Offerer, {std::byte{7}}, 100);
    HandshakeExchange second(borrowed(f.b), b, HandshakeRole::Answerer, {std::byte{8}}, 80);
    EXPECT_TRUE(
        std::holds_alternative<std::monostate>(second.receive(std::get<Bytes>(first.hello()))));
}

TEST(SignalingSession, RejectsReplayWrongPeerAndAttemptWithoutConsumingValidSequence) {
    Fixture f;
    SignalingFields fields{SignalingKind::Candidate,
                           f.peer_a.instance,
                           f.peer_b,
                           f.attempt,
                           0,
                           100,
                           150,
                           {std::byte{9}}};
    auto wrong = std::get<Bytes>(
        encode_signaling(f.other, fields, f.box_b.public_key(), f.capability, f.policy));
    EXPECT_EQ(std::get<AttemptDecision>(f.right.verify(wrong, f.box_b, f.capability, f.policy)),
              AttemptDecision::WrongSender);
    fields.attempt_id[0] = std::byte{8};
    auto wrong_attempt = std::get<Bytes>(
        encode_signaling(f.a, fields, f.box_b.public_key(), f.capability, f.policy));
    EXPECT_EQ(
        std::get<AttemptDecision>(f.right.verify(wrong_attempt, f.box_b, f.capability, f.policy)),
        AttemptDecision::WrongAttempt);
    fields.attempt_id = f.attempt;
    auto valid = std::get<Bytes>(
        encode_signaling(f.a, fields, f.box_b.public_key(), f.capability, f.policy));
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(
        f.right.verify(valid, f.box_b, f.capability, f.policy)));
    EXPECT_EQ(std::get<AttemptDecision>(f.right.verify(valid, f.box_b, f.capability, f.policy)),
              AttemptDecision::Duplicate);
}

TEST(SignalingSession, RejectsWrongCapabilityDescriptionAndExpiredRecords) {
    Fixture f;
    const Bytes payload{std::byte{1}};
    EXPECT_EQ(
        std::get<SessionError>(f.left.encode(f.a, f.box_a.public_key(), f.capability,
                                             SignalingKind::Offer, payload, 100, 150, f.policy)),
        SessionError::InvalidBinding);
    auto invalid_policy = f.policy;
    invalid_policy.max_lifetime = 0;
    EXPECT_EQ(std::get<SignalingError>(f.left.encode(f.a, f.box_b.public_key(), f.capability,
                                                     SignalingKind::Offer, payload, 100, 150,
                                                     invalid_policy)),
              SignalingError::InvalidPolicy);
    EXPECT_EQ(
        std::get<SessionError>(f.left.encode(f.a, f.box_b.public_key(), Bytes(32),
                                             SignalingKind::Offer, payload, 100, 150, f.policy)),
        SessionError::WrongCapability);
    EXPECT_EQ(
        std::get<SessionError>(f.left.encode(f.a, f.box_b.public_key(), f.capability,
                                             SignalingKind::Answer, payload, 100, 150, f.policy)),
        SessionError::WrongKind);
    const auto offer = f.encode(f.left, f.a, f.box_b.public_key(), SignalingKind::Offer, payload);
    EXPECT_EQ(std::get<SessionError>(f.right.verify(offer, f.box_a, f.capability, f.policy)),
              SessionError::InvalidBinding);
    auto expired = f.policy;
    expired.now = 200;
    EXPECT_EQ(std::get<SignalingError>(f.right.verify(offer, f.box_b, f.capability, expired)),
              SignalingError::Expired);
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(
        f.right.verify(offer, f.box_b, f.capability, f.policy)));
    EXPECT_EQ(
        std::get<SessionError>(f.left.encode(f.a, f.box_b.public_key(), f.capability,
                                             SignalingKind::Offer, payload, 100, 150, f.policy)),
        SessionError::DuplicateDescription);
}
