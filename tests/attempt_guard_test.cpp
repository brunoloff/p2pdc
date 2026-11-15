#include "p2pdc/attempt_guard.h"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

using namespace p2pdc;

TEST(AttemptGuard, RejectsMismatchedBindingsWithoutConsumingSequence) {
    SignalingEnvelope envelope{};
    envelope.sender_signing_key[0] = std::byte{1};
    envelope.fields.sender_instance[0] = std::byte{2};
    envelope.fields.attempt_id[0] = std::byte{3};
    AttemptGuard guard(envelope.sender_signing_key, envelope.fields.sender_instance,
                       envelope.fields.attempt_id, 4);
    auto wrong = envelope;
    wrong.sender_signing_key[0] ^= std::byte{1};
    EXPECT_EQ(guard.accept(wrong), AttemptDecision::WrongSender);
    wrong = envelope;
    wrong.fields.sender_instance[0] ^= std::byte{1};
    EXPECT_EQ(guard.accept(wrong), AttemptDecision::WrongInstance);
    wrong = envelope;
    wrong.fields.attempt_id[0] ^= std::byte{1};
    EXPECT_EQ(guard.accept(wrong), AttemptDecision::WrongAttempt);
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Duplicate);
}

TEST(AttemptGuard, PermitsReorderingAndRejectsReplayAndLargeJumps) {
    SignalingEnvelope envelope{};
    AttemptGuard guard({}, {}, {}, 4);
    envelope.fields.sequence = 4;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = 1;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Duplicate);
    envelope.fields.sequence = 9;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::TooFarAhead);
    envelope.fields.sequence = 8;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::TooFarAhead);
}

TEST(AttemptGuard, BoundsInitialSequenceAndWindowAtSixtyFour) {
    EXPECT_THROW((AttemptGuard{{}, {}, {}, 0}), std::invalid_argument);
    EXPECT_THROW((AttemptGuard{{}, {}, {}, 65}), std::invalid_argument);
    AttemptGuard guard({}, {}, {}, 64);
    SignalingEnvelope envelope{};
    envelope.fields.sequence = 65;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::TooFarAhead);
    envelope.fields.sequence = 0;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = 64;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = 0;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::TooOld);
    envelope.fields.sequence = 1;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
}

TEST(AttemptGuard, PreservesOldestSeenBitUntilItLeavesWindow) {
    AttemptGuard guard({}, {}, {}, 64);
    SignalingEnvelope envelope{};
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = 63;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = 0;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Duplicate);
    envelope.fields.sequence = 64;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::Accepted);
    envelope.fields.sequence = 0;
    EXPECT_EQ(guard.accept(envelope), AttemptDecision::TooOld);
}
