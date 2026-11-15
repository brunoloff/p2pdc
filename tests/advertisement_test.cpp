#include "p2pdc/advertisement.h"

#include <gtest/gtest.h>

#include <sodium.h>

#include <limits>
#include <stdexcept>
#include <string_view>

using namespace p2pdc;

namespace {
constexpr std::array<std::byte, capability_size> capability{std::byte{7}};
constexpr AdvertisementPolicy policy{100, 60, 5};

AdvertisementFields fields() {
    return {RecipientPublicKey{std::byte{2}},
            InstanceId{std::byte{3}},
            SignalingInboxKey{std::byte{4}},
            0x0102030405060708ULL,
            9,
            100,
            150};
}

Bytes prefixed(std::string_view domain, ByteView bytes) {
    const auto prefix = std::as_bytes(std::span(domain));
    Bytes result(prefix.begin(), prefix.end());
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}

Bytes bound_signature_input(ByteView body) {
    std::array<unsigned char, 32> context{};
    if (crypto_generichash(
            context.data(), context.size(),
            reinterpret_cast<const unsigned char *>(advertisement_group_domain.data()),
            advertisement_group_domain.size(),
            reinterpret_cast<const unsigned char *>(capability.data()), capability.size()) != 0) {
        throw std::runtime_error("Cannot derive test advertisement group context");
    }
    auto input = prefixed(advertisement_signature_domain, std::as_bytes(std::span(context)));
    input.insert(input.end(), body.begin(), body.end());
    return input;
}

class AdvertisementTest : public ::testing::Test {
  protected:
    Identity identity = std::get<Identity>(Identity::generate());

    Bytes wire() const {
        return std::get<Bytes>(encode_advertisement(identity, fields(), capability, policy));
    }

    Bytes plaintext(ByteView wire_bytes) const {
        const auto aad =
            prefixed(advertisement_aead_domain, wire_bytes.first(advertisement_header_size));
        return std::get<Bytes>(decrypt_payload(
            capability, wire_bytes.subspan(advertisement_header_size, encryption_nonce_size),
            wire_bytes.subspan(advertisement_header_size + encryption_nonce_size), aad,
            advertisement_plaintext_size));
    }

    Bytes reseal(ByteView bytes) const {
        const auto original = wire();
        const auto header = ByteView(original).first(advertisement_header_size);
        const auto encrypted = std::get<EncryptedPayload>(encrypt_payload(
            capability, bytes, prefixed(advertisement_aead_domain, header), bytes.size()));
        Bytes result(header.begin(), header.end());
        result.insert(result.end(), encrypted.nonce.begin(), encrypted.nonce.end());
        result.insert(result.end(), encrypted.ciphertext.begin(), encrypted.ciphertext.end());
        return result;
    }

    Bytes resign(Bytes bytes) const {
        const auto signature = std::get<Signature>(
            identity.sign(bound_signature_input(ByteView(bytes).first(advertisement_body_size))));
        std::copy(signature.begin(), signature.end(), bytes.begin() + advertisement_body_size);
        return reseal(bytes);
    }
};
} // namespace

TEST(AdvertisementVector, DecodesIndependentEncryptedRecordAndRejectsTampering) {
    constexpr std::string_view vector =
#include "advertisement_vector.inc"
        ;
    Bytes wire(vector.size() / 2);
    ASSERT_EQ(sodium_hex2bin(reinterpret_cast<unsigned char *>(wire.data()), wire.size(),
                             vector.data(), vector.size(), nullptr, nullptr, nullptr),
              0);
    auto identity = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    const auto decoded = decode_advertisement(wire, capability, policy);
    ASSERT_TRUE(std::holds_alternative<Advertisement>(decoded));
    EXPECT_EQ(std::get<Advertisement>(decoded), (Advertisement{identity.public_key(), fields()}));
    wire.back() ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(wire, capability, policy)),
              CryptoError::AuthenticationFailed);
}

TEST_F(AdvertisementTest, RoundTripsAllFieldsAndCanonicalBigEndianIntegers) {
    const auto encoded = wire();
    ASSERT_EQ(encoded.size(), advertisement_wire_size);
    const auto decoded = decode_advertisement(encoded, capability, policy);
    ASSERT_TRUE(std::holds_alternative<Advertisement>(decoded));
    EXPECT_EQ(std::get<Advertisement>(decoded), (Advertisement{identity.public_key(), fields()}));
    const auto body = plaintext(encoded);
    EXPECT_EQ(body.size(), advertisement_plaintext_size);
    EXPECT_EQ(body[0], std::byte{1});
    EXPECT_EQ(body[101], std::byte{1});
    EXPECT_EQ(body[108], std::byte{8});
    EXPECT_EQ(body[116], std::byte{9});
    EXPECT_EQ(body[124], std::byte{100});
    EXPECT_EQ(body[132], std::byte{150});
    const auto repeated = wire();
    EXPECT_NE(encoded, repeated);
    EXPECT_EQ(plaintext(encoded), plaintext(repeated));
}

TEST_F(AdvertisementTest, RejectsEveryTruncatedLengthAndTrailingData) {
    const auto encoded = wire();
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        EXPECT_EQ(std::get<AdvertisementError>(
                      decode_advertisement(ByteView(encoded).first(length), capability, policy)),
                  AdvertisementError::InvalidLength);
    }
    auto extended = encoded;
    extended.push_back(std::byte{0});
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(extended, capability, policy)),
              AdvertisementError::InvalidLength);
}

TEST_F(AdvertisementTest, RejectsMagicAndOuterVersionChanges) {
    auto encoded = wire();
    encoded[0] ^= std::byte{1};
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(encoded, capability, policy)),
              AdvertisementError::InvalidMagic);
    encoded = wire();
    encoded[4] = std::byte{2};
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(encoded, capability, policy)),
              AdvertisementError::UnsupportedVersion);
}

TEST_F(AdvertisementTest, RejectsWrongCapabilityMalformedCapabilityAndMovedIdentity) {
    auto wrong = capability;
    wrong[0] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(wire(), wrong, policy)),
              CryptoError::AuthenticationFailed);
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(wire(), {}, policy)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(encode_advertisement(identity, fields(), {}, policy)),
              CryptoError::InvalidLength);
    auto moved = std::move(identity);
    // Deliberately verify rejection of a moved-from signing identity.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(std::get<CryptoError>(encode_advertisement(identity, fields(), capability, policy)),
              CryptoError::InvalidIdentity);
    EXPECT_TRUE(
        std::holds_alternative<Bytes>(encode_advertisement(moved, fields(), capability, policy)));
}

TEST_F(AdvertisementTest, RejectsNonceCiphertextAndTagTampering) {
    for (const auto offset :
         {advertisement_header_size, advertisement_header_size + encryption_nonce_size,
          advertisement_wire_size - 1}) {
        auto encoded = wire();
        encoded[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<CryptoError>(decode_advertisement(encoded, capability, policy)),
                  CryptoError::AuthenticationFailed);
    }
}

TEST_F(AdvertisementTest, RejectsValidAdvertisementRewrappedForAnotherCapability) {
    const auto original = wire();
    const auto signed_plaintext = plaintext(original);
    auto other_capability = capability;
    other_capability[0] ^= std::byte{1};
    const auto header = ByteView(original).first(advertisement_header_size);
    const auto aad = prefixed(advertisement_aead_domain, header);
    const auto encrypted = std::get<EncryptedPayload>(
        encrypt_payload(other_capability, signed_plaintext, aad, advertisement_plaintext_size));
    const auto decrypted = decrypt_payload(other_capability, encrypted.nonce, encrypted.ciphertext,
                                           aad, advertisement_plaintext_size);
    ASSERT_TRUE(std::holds_alternative<Bytes>(decrypted));
    EXPECT_EQ(std::get<Bytes>(decrypted), signed_plaintext);
    Bytes rewrapped(header.begin(), header.end());
    rewrapped.insert(rewrapped.end(), encrypted.nonce.begin(), encrypted.nonce.end());
    rewrapped.insert(rewrapped.end(), encrypted.ciphertext.begin(), encrypted.ciphertext.end());
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(rewrapped, other_capability, policy)),
              CryptoError::AuthenticationFailed);
    EXPECT_TRUE(
        std::holds_alternative<Advertisement>(decode_advertisement(original, capability, policy)));
    const auto other_wire =
        std::get<Bytes>(encode_advertisement(identity, fields(), other_capability, policy));
    EXPECT_TRUE(std::holds_alternative<Advertisement>(
        decode_advertisement(other_wire, other_capability, policy)));
}

TEST_F(AdvertisementTest, RejectsEachSignedFieldChangedByCapabilityHolder) {
    const auto original = plaintext(wire());
    for (const auto offset : {1U, 33U, 65U, 81U, 101U, 109U, 117U, 125U, 133U, 196U}) {
        auto changed = original;
        changed[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<CryptoError>(decode_advertisement(reseal(changed), capability, policy)),
                  CryptoError::AuthenticationFailed);
    }
    auto changed = original;
    changed[0] = std::byte{2};
    EXPECT_EQ(
        std::get<AdvertisementError>(decode_advertisement(reseal(changed), capability, policy)),
        AdvertisementError::UnsupportedVersion);
}

TEST_F(AdvertisementTest, RejectsSignatureFromDifferentDomain) {
    auto body = plaintext(wire());
    const auto signature =
        std::get<Signature>(identity.sign(ByteView(body).first(advertisement_body_size)));
    std::copy(signature.begin(), signature.end(), body.begin() + advertisement_body_size);
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(reseal(body), capability, policy)),
              CryptoError::AuthenticationFailed);
}

TEST_F(AdvertisementTest, RejectsDifferentAssociatedDataDomain) {
    const auto bytes = plaintext(wire());
    const auto encoded = wire();
    const auto header = ByteView(encoded).first(advertisement_header_size);
    const auto encrypted = std::get<EncryptedPayload>(encrypt_payload(
        capability, bytes, prefixed("other-protocol", header), advertisement_plaintext_size));
    Bytes changed(header.begin(), header.end());
    changed.insert(changed.end(), encrypted.nonce.begin(), encrypted.nonce.end());
    changed.insert(changed.end(), encrypted.ciphertext.begin(), encrypted.ciphertext.end());
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(changed, capability, policy)),
              CryptoError::AuthenticationFailed);
}

TEST_F(AdvertisementTest, ChecksExclusiveExpiryAndClockSkewBoundaries) {
    const auto encoded = wire();
    for (const auto now : {95U, 100U, 150U, 154U}) {
        EXPECT_TRUE(std::holds_alternative<Advertisement>(
            decode_advertisement(encoded, capability, {now, 60, 5})));
    }
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(encoded, capability, {94, 60, 5})),
              AdvertisementError::NotYetValid);
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(encoded, capability, {155, 60, 5})),
              AdvertisementError::Expired);
    EXPECT_TRUE(std::holds_alternative<Advertisement>(
        decode_advertisement(encoded, capability, {149, 60, 0})));
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(encoded, capability, {150, 60, 0})),
              AdvertisementError::Expired);
}

TEST_F(AdvertisementTest, RejectsInvalidLifetimeAndPoliciesOnBothBoundaries) {
    auto invalid = fields();
    for (const auto expiry : {99U, 100U, 161U}) {
        invalid.expires_at = expiry;
        EXPECT_EQ(std::get<AdvertisementError>(
                      encode_advertisement(identity, invalid, capability, policy)),
                  AdvertisementError::InvalidLifetime);
    }
    EXPECT_EQ(std::get<AdvertisementError>(
                  encode_advertisement(identity, fields(), capability, {100, 0, 5})),
              AdvertisementError::InvalidPolicy);
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(wire(), capability, {100, 0, 5})),
              AdvertisementError::InvalidPolicy);
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(wire(), capability, {100, 49, 5})),
              AdvertisementError::InvalidLifetime);
    EXPECT_TRUE(std::holds_alternative<Advertisement>(
        decode_advertisement(wire(), capability, {100, 50, 5})));
    auto bytes = plaintext(wire());
    std::fill(bytes.begin() + 125, bytes.begin() + 133, std::byte{0});
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(resign(bytes), capability, policy)),
              AdvertisementError::InvalidLifetime);
}

TEST_F(AdvertisementTest, EncoderChecksFutureAndExpiredInputs) {
    EXPECT_EQ(std::get<AdvertisementError>(
                  encode_advertisement(identity, fields(), capability, {94, 60, 5})),
              AdvertisementError::NotYetValid);
    EXPECT_EQ(std::get<AdvertisementError>(
                  encode_advertisement(identity, fields(), capability, {155, 60, 5})),
              AdvertisementError::Expired);
}

TEST_F(AdvertisementTest, HandlesUint64BoundariesWithoutWrapping) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto boundary = fields();
    boundary.issued_at = maximum - 10;
    boundary.expires_at = maximum;
    auto encoded = std::get<Bytes>(
        encode_advertisement(identity, boundary, capability, {maximum - 10, 10, 5}));
    EXPECT_TRUE(std::holds_alternative<Advertisement>(
        decode_advertisement(encoded, capability, {maximum, 10, 5})));
    EXPECT_EQ(
        std::get<AdvertisementError>(decode_advertisement(encoded, capability, {maximum, 10, 0})),
        AdvertisementError::Expired);
    EXPECT_EQ(std::get<AdvertisementError>(decode_advertisement(encoded, capability, {0, 10, 5})),
              AdvertisementError::NotYetValid);
    boundary.issued_at = 0;
    boundary.expires_at = 1;
    encoded = std::get<Bytes>(encode_advertisement(identity, boundary, capability, {0, 1, 0}));
    EXPECT_EQ(std::get<AdvertisementError>(
                  decode_advertisement(encoded, capability, {maximum, 1, maximum - 1})),
              AdvertisementError::Expired);
    EXPECT_TRUE(std::holds_alternative<Advertisement>(
        decode_advertisement(encoded, capability, {maximum, 1, maximum})));
}
