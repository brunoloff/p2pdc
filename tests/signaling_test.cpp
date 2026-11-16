#include "../src/security/protocol_signature.hpp"
#include "../src/security/signaling_limits.hpp"
#include "p2pdc/signaling.h"

#include <gtest/gtest.h>

#include <sodium.h>

#include <limits>
#include <stdexcept>

using namespace p2pdc;

namespace {
constexpr std::array<std::byte, capability_size> capability{std::byte{9}};
constexpr SignalingPolicy policy{100, 60, 5, 1024};

Bytes signature_input(ByteView body) {
    std::array<unsigned char, 32> context{};
    if (crypto_generichash(context.data(), context.size(),
                           reinterpret_cast<const unsigned char *>(signaling_group_domain.data()),
                           signaling_group_domain.size(),
                           reinterpret_cast<const unsigned char *>(capability.data()),
                           capability.size()) != 0) {
        throw std::runtime_error("Cannot derive signaling test group context");
    }
    const auto domain = std::as_bytes(std::span(signaling_signature_domain));
    Bytes input(domain.begin(), domain.end());
    const auto group = std::as_bytes(std::span(context));
    input.insert(input.end(), group.begin(), group.end());
    input.insert(input.end(), body.begin(), body.end());
    return input;
}

class SignalingTest : public ::testing::Test {
  protected:
    Identity sender = std::get<Identity>(
        Identity::import_seed(std::array<std::byte, identity_seed_size>{std::byte{7}}));
    Identity receiver = std::get<Identity>(
        Identity::import_seed(std::array<std::byte, identity_seed_size>{std::byte{8}}));
    RecipientKeyPair recipient = std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(
        std::array<std::byte, recipient_box_secret_key_size>{std::byte{17}}));

    SignalingRecipient expected() const { return {receiver.public_key(), {std::byte{2}}}; }

    SignalingFields fields() const {
        return {SignalingKind::Offer,
                {std::byte{1}},
                expected(),
                {std::byte{3}},
                0x0102030405060708ULL,
                100,
                150,
                {std::byte{0}, std::byte{0xff}, std::byte{7}}};
    }

    Bytes wire() const {
        return std::get<Bytes>(
            encode_signaling(sender, fields(), recipient.public_key(), capability, policy));
    }

    Bytes plaintext() const {
        const auto encoded = wire();
        return std::get<Bytes>(
            recipient.open_sealed(ByteView(encoded).subspan(signaling_header_size),
                                  signaling_signed_overhead + policy.max_payload_size));
    }

    Bytes reseal(ByteView bytes) const {
        const auto original = wire();
        const auto encrypted =
            std::get<Bytes>(seal_to_recipient(recipient.public_key(), bytes, bytes.size()));
        Bytes result(original.begin(), original.begin() + signaling_header_size);
        result.insert(result.end(), encrypted.begin(), encrypted.end());
        return result;
    }

    Bytes resign(Bytes bytes) const {
        const auto body = ByteView(bytes).first(bytes.size() - signature_size);
        const auto signature =
            std::get<Signature>(detail::ProtocolSigning::sign(sender, signature_input(body)));
        std::copy(signature.begin(), signature.end(), bytes.end() - signature_size);
        return reseal(bytes);
    }

    SignalingResult<SignalingEnvelope> decode(ByteView bytes,
                                              SignalingPolicy limits = policy) const {
        return decode_signaling(bytes, recipient, expected(), capability, limits);
    }
};
} // namespace

TEST_F(SignalingTest, RoundTripsOfferAndCandidateAndCanonicalFields) {
    for (const auto kind : {SignalingKind::Offer, SignalingKind::Answer, SignalingKind::Candidate,
                            SignalingKind::Cancel, SignalingKind::Reject}) {
        auto input = fields();
        input.kind = kind;
        const auto encoded = std::get<Bytes>(
            encode_signaling(sender, input, recipient.public_key(), capability, policy));
        EXPECT_EQ(encoded.size(), signaling_wire_overhead + input.payload.size());
        const auto decoded = decode(encoded);
        ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(decoded));
        EXPECT_EQ(std::get<SignalingEnvelope>(decoded),
                  (SignalingEnvelope{sender.public_key(), input}));
    }
    const auto body = plaintext();
    EXPECT_EQ(body[118], std::byte{1});
    EXPECT_EQ(body[125], std::byte{8});
    EXPECT_EQ(body[133], std::byte{100});
    EXPECT_EQ(body[141], std::byte{150});
    EXPECT_EQ(body[145], std::byte{3});
}

TEST_F(SignalingTest, SupportsEmptyPayloadWithZeroPayloadLimit) {
    auto input = fields();
    input.payload.clear();
    const SignalingPolicy limits{100, 60, 0, 0};
    const auto encoded = std::get<Bytes>(
        encode_signaling(sender, input, recipient.public_key(), capability, limits));
    EXPECT_EQ(encoded.size(), signaling_wire_overhead);
    const auto decoded = decode(encoded, limits);
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(decoded));
    EXPECT_TRUE(std::get<SignalingEnvelope>(decoded).fields.payload.empty());
}

TEST_F(SignalingTest, BindsEverySignedFieldAndRejectsSignatureSubstitution) {
    const auto original = plaintext();
    for (const auto offset : {5U, 6U, 38U, 70U, 86U, 102U, 118U, 126U, 134U, 146U, 149U}) {
        auto altered = original;
        altered[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<CryptoError>(decode(reseal(altered))),
                  CryptoError::AuthenticationFailed);
    }
    auto altered = original;
    const auto signature = std::get<Signature>(detail::ProtocolSigning::sign(
        receiver, signature_input(ByteView(altered).first(altered.size() - signature_size))));
    std::copy(signature.begin(), signature.end(), altered.end() - signature_size);
    EXPECT_EQ(std::get<CryptoError>(decode(reseal(altered))), CryptoError::AuthenticationFailed);
}

TEST_F(SignalingTest, RejectsRewrappedMessageFromAnotherCapability) {
    const auto body = plaintext();
    const auto rewrapped = reseal(body);
    auto other = capability;
    other[0] ^= std::byte{1};
    const auto opened = recipient.open_sealed(ByteView(rewrapped).subspan(signaling_header_size),
                                              signaling_signed_overhead + policy.max_payload_size);
    ASSERT_TRUE(std::holds_alternative<Bytes>(opened));
    EXPECT_EQ(std::get<Bytes>(opened), body);
    EXPECT_EQ(
        std::get<CryptoError>(decode_signaling(rewrapped, recipient, expected(), other, policy)),
        CryptoError::AuthenticationFailed);
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(decode(rewrapped)));
    const auto legitimate =
        std::get<Bytes>(encode_signaling(sender, fields(), recipient.public_key(), other, policy));
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(
        decode_signaling(legitimate, recipient, expected(), other, policy)));
}

TEST_F(SignalingTest, RejectsWrongRecipientKeyIdentityAndInstance) {
    auto other_key = std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(
        std::array<std::byte, recipient_box_secret_key_size>{std::byte{42}}));
    EXPECT_EQ(
        std::get<CryptoError>(decode_signaling(wire(), other_key, expected(), capability, policy)),
        CryptoError::AuthenticationFailed);
    auto other_identity = expected();
    other_identity.signing_key = sender.public_key();
    EXPECT_EQ(std::get<SignalingError>(
                  decode_signaling(wire(), recipient, other_identity, capability, policy)),
              SignalingError::RecipientMismatch);
    auto other_instance = expected();
    other_instance.instance[0] ^= std::byte{1};
    EXPECT_EQ(std::get<SignalingError>(
                  decode_signaling(wire(), recipient, other_instance, capability, policy)),
              SignalingError::RecipientMismatch);
}

TEST_F(SignalingTest, RejectsCiphertextTamperShortWireAndTrailingData) {
    const auto encoded = wire();
    for (const auto offset :
         {signaling_header_size, signaling_header_size + 32, encoded.size() - 1}) {
        auto altered = encoded;
        altered[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<CryptoError>(decode(altered)), CryptoError::AuthenticationFailed);
    }
    for (std::size_t length = 0; length < signaling_wire_overhead; ++length) {
        EXPECT_EQ(std::get<SignalingError>(decode(ByteView(encoded).first(length))),
                  SignalingError::InvalidLength);
    }
    auto extended = encoded;
    extended.push_back(std::byte{0});
    EXPECT_EQ(std::get<CryptoError>(decode(extended)), CryptoError::AuthenticationFailed);
}

TEST_F(SignalingTest, RejectsOuterAndEncryptedMagicVersionsAndUnknownKind) {
    for (const auto offset : {0U, 4U}) {
        auto altered = wire();
        altered[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<SignalingError>(decode(altered)),
                  offset == 0 ? SignalingError::InvalidMagic : SignalingError::UnsupportedVersion);
        altered = plaintext();
        altered[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<SignalingError>(decode(resign(altered))),
                  offset == 0 ? SignalingError::InvalidMagic : SignalingError::UnsupportedVersion);
    }
    auto input = fields();
    input.kind = SignalingKind::Unknown;
    EXPECT_EQ(std::get<SignalingError>(
                  encode_signaling(sender, input, recipient.public_key(), capability, policy)),
              SignalingError::InvalidKind);
    auto body = plaintext();
    body[5] = std::byte{255};
    EXPECT_EQ(std::get<SignalingError>(decode(resign(body))), SignalingError::InvalidKind);
}

TEST_F(SignalingTest, RejectsCanonicalLengthMismatchAndHonorsSizeBounds) {
    auto body = plaintext();
    body[145] = std::byte{4};
    EXPECT_EQ(std::get<SignalingError>(decode(resign(body))), SignalingError::InvalidLength);
    body[145] = std::byte{2};
    EXPECT_EQ(std::get<SignalingError>(decode(resign(body))), SignalingError::InvalidLength);
    const auto short_limit = SignalingPolicy{100, 60, 5, 2};
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), short_limit)), SignalingError::TooLarge);
    EXPECT_EQ(std::get<SignalingError>(encode_signaling(sender, fields(), recipient.public_key(),
                                                        capability, short_limit)),
              SignalingError::TooLarge);
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(decode(wire(), {100, 60, 5, 3})));
    auto extended_body = plaintext();
    extended_body.insert(extended_body.end() - signature_size, std::byte{0});
    EXPECT_EQ(std::get<SignalingError>(decode(resign(extended_body))),
              SignalingError::InvalidLength);
}

TEST_F(SignalingTest, RejectsExpiredFutureAndExcessiveLifetime) {
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(decode(wire(), {95, 60, 5, 3})));
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), {94, 60, 5, 3})),
              SignalingError::NotYetValid);
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(decode(wire(), {154, 60, 5, 3})));
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), {155, 60, 5, 3})), SignalingError::Expired);
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(decode(wire(), {149, 60, 0, 3})));
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), {150, 60, 0, 3})), SignalingError::Expired);
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), {100, 49, 5, 3})),
              SignalingError::InvalidLifetime);
    auto input = fields();
    for (const auto expiry : {99U, 100U, 161U}) {
        input.expires_at = expiry;
        EXPECT_EQ(std::get<SignalingError>(
                      encode_signaling(sender, input, recipient.public_key(), capability, policy)),
                  SignalingError::InvalidLifetime);
    }
    auto body = plaintext();
    std::fill(body.begin() + 134, body.begin() + 142, std::byte{0});
    EXPECT_EQ(std::get<SignalingError>(decode(resign(body))), SignalingError::InvalidLifetime);
}

TEST_F(SignalingTest, ChecksPolicyCapabilityAndSenderErrors) {
    auto invalid = policy;
    invalid.max_lifetime = 0;
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), invalid)), SignalingError::InvalidPolicy);
    EXPECT_EQ(std::get<SignalingError>(
                  encode_signaling(sender, fields(), recipient.public_key(), capability, invalid)),
              SignalingError::InvalidPolicy);
    invalid = policy;
    invalid.max_payload_size = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(std::get<SignalingError>(decode(wire(), invalid)), SignalingError::InvalidPolicy);
    EXPECT_EQ(std::get<CryptoError>(decode_signaling(wire(), recipient, expected(), {}, policy)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(
                  encode_signaling(sender, fields(), recipient.public_key(), {}, policy)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(
                  encode_signaling(sender, fields(), RecipientBoxPublicKey{}, capability, policy)),
              CryptoError::OperationFailed);
    auto moved = std::move(sender);
    // Deliberately exercise the defined moved-from rejection contract.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(std::get<CryptoError>(
                  encode_signaling(sender, fields(), recipient.public_key(), capability, policy)),
              CryptoError::InvalidIdentity);
    EXPECT_TRUE(std::holds_alternative<Bytes>(
        encode_signaling(moved, fields(), recipient.public_key(), capability, policy)));
}

TEST_F(SignalingTest, HandlesMaximumUtcTimesWithoutOverflow) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto input = fields();
    input.issued_at = maximum - 10;
    input.expires_at = maximum;
    const auto encoded = std::get<Bytes>(encode_signaling(sender, input, recipient.public_key(),
                                                          capability, {maximum - 10, 10, 5, 3}));
    EXPECT_TRUE(std::holds_alternative<SignalingEnvelope>(decode(encoded, {maximum, 10, 5, 3})));
    EXPECT_EQ(std::get<SignalingError>(decode(encoded, {maximum, 10, 0, 3})),
              SignalingError::Expired);
    EXPECT_EQ(std::get<SignalingError>(decode(encoded, {0, 10, 5, 3})),
              SignalingError::NotYetValid);
    input.issued_at = 0;
    input.expires_at = 1;
    const auto earliest = std::get<Bytes>(
        encode_signaling(sender, input, recipient.public_key(), capability, {0, 1, 0, 3}));
    EXPECT_EQ(std::get<SignalingError>(decode(earliest, {maximum, 1, maximum - 1, 3})),
              SignalingError::Expired);
    EXPECT_TRUE(
        std::holds_alternative<SignalingEnvelope>(decode(earliest, {maximum, 1, maximum, 3})));
}

TEST_F(SignalingTest, RejectsSignaturesWithDifferentDomain) {
    auto body = plaintext();
    const auto signature = std::get<Signature>(
        detail::ProtocolSigning::sign(sender, ByteView(body).first(body.size() - signature_size)));
    std::copy(signature.begin(), signature.end(), body.end() - signature_size);
    EXPECT_EQ(std::get<CryptoError>(decode(reseal(body))), CryptoError::AuthenticationFailed);
}

TEST_F(SignalingTest, StatelessCodecReturnsReplayedRecordsForHigherLevelDeduplication) {
    const auto encoded = wire();
    const auto first = decode(encoded);
    const auto replay = decode(encoded);
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(first));
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(replay));
    EXPECT_EQ(std::get<SignalingEnvelope>(first), std::get<SignalingEnvelope>(replay));
}

TEST_F(SignalingTest, ReturnsAlternateSenderInstanceAndAttemptForHigherLevelAuthorization) {
    auto alternate = std::get<Identity>(
        Identity::import_seed(std::array<std::byte, identity_seed_size>{std::byte{19}}));
    auto input = fields();
    input.sender_instance[0] = std::byte{9};
    input.attempt_id[0] = std::byte{8};
    input.sequence = 2;
    const auto encoded = std::get<Bytes>(
        encode_signaling(alternate, input, recipient.public_key(), capability, policy));
    const auto decoded = decode(encoded);
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(decoded));
    EXPECT_EQ(std::get<SignalingEnvelope>(decoded),
              (SignalingEnvelope{alternate.public_key(), input}));
}

TEST_F(SignalingTest, MatchesIndependentlyDerivedCanonicalBodyAndSignatureVector) {
    // Derived independently using Python struct.pack('>QQQI') and direct libsodium
    // Ed25519 calls with fixed seed bytes 07/08 and capability 09 followed by zeros.
    constexpr std::string_view vector =
#include "signaling_vector.inc"
        ;
    Bytes expected_bytes(vector.size() / 2);
    ASSERT_EQ(sodium_hex2bin(reinterpret_cast<unsigned char *>(expected_bytes.data()),
                             expected_bytes.size(), vector.data(), vector.size(), nullptr, nullptr,
                             nullptr),
              0);
    EXPECT_EQ(plaintext(), expected_bytes);
}

TEST_F(SignalingTest, MailboxWireLimitIncludesEncryptionAndSignatureOverhead) {
    auto input = fields();
    auto limit = policy;
    limit.max_payload_size = detail::signaling_payload_limit;
    input.payload = Bytes(detail::signaling_payload_limit, std::byte{42});
    auto encoded = encode_signaling(sender, input, recipient.public_key(), capability, limit);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    EXPECT_EQ(std::get<Bytes>(encoded).size(), detail::signaling_wire_limit);
    input.payload.push_back(std::byte{42});
    auto oversized = encode_signaling(sender, input, recipient.public_key(), capability, limit);
    ASSERT_TRUE(std::holds_alternative<SignalingError>(oversized));
    EXPECT_EQ(std::get<SignalingError>(oversized), SignalingError::TooLarge);
}

TEST_F(SignalingTest, EmptyPayloadRecordIsExactlyTheWireOverhead) {
    auto input = fields();
    input.kind = SignalingKind::Cancel;
    input.payload.clear();
    auto encoded = encode_signaling(sender, input, recipient.public_key(), capability, policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    EXPECT_EQ(std::get<Bytes>(encoded).size(), signaling_wire_overhead);
}
