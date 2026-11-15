#include "../src/discovery/incoming_offer_resolver.hpp"

#include <gtest/gtest.h>

#include <limits>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
constexpr std::array<std::byte, capability_size> capability{std::byte{9}};
constexpr IncomingOfferLimits limits{2, 60, 60, 5, 1024, 8};

class IncomingOfferTest : public ::testing::Test {
  protected:
    Identity sender = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity receiver = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    RecipientKeyPair sender_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    RecipientKeyPair recipient_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    SignalingRecipient recipient{receiver.public_key(), {std::byte{5}}};
    IncomingOfferResolver resolver{limits};

    Advertisement peer(std::byte inbox = std::byte{6}, std::uint64_t expiry = 150) const {
        AdvertisementFields fields{
            sender_box.public_key(), {std::byte{7}}, {inbox}, 1, 0, 100, expiry};
        auto encoded = encode_advertisement(sender, fields, capability, {100, 60, 5});
        return std::get<Advertisement>(
            decode_advertisement(std::get<Bytes>(encoded), capability, {100, 60, 5}));
    }
    Bytes wire(std::byte attempt = std::byte{8}, SignalingKind kind = SignalingKind::Offer,
               std::uint64_t sequence = 0) const {
        SignalingFields fields{kind,      {std::byte{7}},
                               recipient, {attempt},
                               sequence,  100,
                               150,       {std::byte{0}, std::byte{255}}};
        return std::get<Bytes>(encode_signaling(sender, fields, recipient_box.public_key(),
                                                capability, {100, 60, 5, 1024}));
    }
    IncomingOfferResult resolve(ByteView value, std::span<const Advertisement> peers,
                                std::uint64_t now = 100) {
        return resolver.resolve(value, recipient_box, recipient, capability, peers, now);
    }
};
} // namespace

TEST_F(IncomingOfferTest, MatchesVerifiedPeerAndSuppressesDuplicateAttempt) {
    const std::array peers{peer()};
    const auto encoded = wire();
    auto result = resolve(encoded, peers);
    ASSERT_TRUE(std::holds_alternative<IncomingOffer>(result));
    const auto &offer = std::get<IncomingOffer>(result);
    EXPECT_EQ(offer.peer, peers.front());
    EXPECT_EQ(offer.envelope.fields.payload, (Bytes{std::byte{0}, std::byte{255}}));
    EXPECT_EQ(offer.wire, encoded);
    EXPECT_EQ(std::get<IncomingOfferError>(resolve(encoded, peers)), IncomingOfferError::Duplicate);
    EXPECT_EQ(resolver.size(), 1U);
}

TEST_F(IncomingOfferTest, ReplayHistorySurvivesReplacingTheResolver) {
    auto history = std::make_shared<IncomingOfferReplay>();
    IncomingOfferResolver first(limits, history), next(limits, history);
    const std::array peers{peer()};
    const auto old = wire();
    ASSERT_TRUE(std::holds_alternative<IncomingOffer>(
        first.resolve(old, recipient_box, recipient, capability, peers, 100)));
    EXPECT_EQ(std::get<IncomingOfferError>(
                  next.resolve(old, recipient_box, recipient, capability, peers, 100)),
              IncomingOfferError::Duplicate);
    EXPECT_TRUE(std::holds_alternative<IncomingOffer>(
        next.resolve(wire(std::byte{9}), recipient_box, recipient, capability, peers, 100)));
    EXPECT_EQ(std::get<IncomingOfferError>(first.resolve(wire(std::byte{10}), recipient_box,
                                                         recipient, capability, peers, 100)),
              IncomingOfferError::Full);
    EXPECT_EQ(next.prune(99), IncomingOfferError::ClockRegressed);
    EXPECT_FALSE(next.prune(171));
    EXPECT_EQ(first.size(), 0U);
}

TEST_F(IncomingOfferTest, UnknownPeerCanBeRetriedAfterDiscovery) {
    const auto encoded = wire();
    EXPECT_EQ(std::get<IncomingOfferError>(resolve(encoded, {})), IncomingOfferError::UnknownPeer);
    EXPECT_EQ(resolver.size(), 0U);
    const std::array peers{peer()};
    EXPECT_TRUE(std::holds_alternative<IncomingOffer>(resolve(encoded, peers)));
}

TEST_F(IncomingOfferTest, RejectsTamperingExpiryNonOfferAndInvalidAttempt) {
    const std::array peers{peer()};
    auto tampered = wire();
    tampered.back() ^= std::byte{1};
    EXPECT_TRUE(std::holds_alternative<CryptoError>(resolve(tampered, peers)));
    EXPECT_EQ(
        std::get<IncomingOfferError>(resolve(wire(std::byte{8}, SignalingKind::Candidate), peers)),
        IncomingOfferError::NotOffer);
    EXPECT_EQ(std::get<IncomingOfferError>(resolve(wire(std::byte{0}), peers)),
              IncomingOfferError::InvalidAttempt);
    EXPECT_EQ(
        std::get<IncomingOfferError>(resolve(wire(std::byte{8}, SignalingKind::Offer, 9), peers)),
        IncomingOfferError::InvalidSequence);
    EXPECT_EQ(std::get<SignalingError>(resolve(wire(), peers, 155)), SignalingError::Expired);
    EXPECT_EQ(resolver.size(), 0U);
}

TEST_F(IncomingOfferTest, RejectsAmbiguousPeerAndEnforcesBoundedReplayRetention) {
    const std::array ambiguous{peer(std::byte{6}), peer(std::byte{10})};
    EXPECT_EQ(std::get<IncomingOfferError>(resolve(wire(), ambiguous)),
              IncomingOfferError::AmbiguousPeer);
    const std::array peers{peer()};
    EXPECT_TRUE(std::holds_alternative<IncomingOffer>(resolve(wire(std::byte{8}), peers)));
    EXPECT_TRUE(std::holds_alternative<IncomingOffer>(resolve(wire(std::byte{9}), peers)));
    EXPECT_EQ(std::get<IncomingOfferError>(resolve(wire(std::byte{10}), peers)),
              IncomingOfferError::Full);
    EXPECT_EQ(resolver.prune(99), IncomingOfferError::ClockRegressed);
    EXPECT_FALSE(resolver.prune(170));
    EXPECT_EQ(resolver.size(), 2U);
    EXPECT_FALSE(resolver.prune(171));
    EXPECT_EQ(resolver.size(), 0U);
}

TEST_F(IncomingOfferTest, RejectsChangesToBoundRecipientAndGroup) {
    const std::array peers{peer()};
    const auto encoded = wire();
    ASSERT_TRUE(std::holds_alternative<IncomingOffer>(resolve(encoded, peers)));
    auto other_capability = capability;
    other_capability[0] ^= std::byte{1};
    EXPECT_EQ(std::get<IncomingOfferError>(resolver.resolve(encoded, recipient_box, recipient,
                                                            other_capability, peers, 100)),
              IncomingOfferError::ContextMismatch);
    auto other_recipient = recipient;
    other_recipient.instance[0] ^= std::byte{1};
    EXPECT_EQ(std::get<IncomingOfferError>(resolver.resolve(encoded, recipient_box, other_recipient,
                                                            capability, peers, 100)),
              IncomingOfferError::ContextMismatch);
    EXPECT_EQ(std::get<IncomingOfferError>(
                  resolver.resolve(encoded, sender_box, recipient, capability, peers, 100)),
              IncomingOfferError::ContextMismatch);
    EXPECT_EQ(resolver.size(), 1U);
}

TEST_F(IncomingOfferTest, RejectsStalePeerAndDifferentCapability) {
    const auto encoded = wire();
    const std::array stale{peer(std::byte{6}, 120)};
    EXPECT_EQ(std::get<IncomingOfferError>(resolve(encoded, stale, 130)),
              IncomingOfferError::InvalidAdvertisement);
    const std::array peers{peer()};
    auto wrong_capability = capability;
    wrong_capability[0] ^= std::byte{1};
    auto result = resolver.resolve(encoded, recipient_box, recipient, wrong_capability, peers, 130);
    EXPECT_TRUE(std::holds_alternative<CryptoError>(result));
    EXPECT_EQ(resolver.size(), 0U);
}

TEST(IncomingOfferValidation, RejectsInvalidLimits) {
    EXPECT_THROW((IncomingOfferResolver{{0, 60, 60, 5, 1024, 8}}), std::invalid_argument);
    EXPECT_THROW((IncomingOfferResolver{{2, 60, 60, 5, 1024, 65}}), std::invalid_argument);
    EXPECT_THROW(
        (IncomingOfferResolver{{2, 60, 60, 5, std::numeric_limits<std::size_t>::max(), 8}}),
        std::invalid_argument);
}
