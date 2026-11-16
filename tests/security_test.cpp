#include "../src/security/protocol_signature.hpp"
#include "p2pdc/security.h"

#include <gtest/gtest.h>

#include <sodium.h>

#include <stdexcept>
#include <string>
#include <type_traits>

using namespace p2pdc;

namespace {
constexpr std::array message{std::byte{0}, std::byte{0xff}, std::byte{7}};
constexpr std::array associated_data{std::byte{9}, std::byte{3}};
const std::array<std::byte, capability_size> capability{std::byte{1}};

Identity generate_identity() { return std::get<Identity>(Identity::generate()); }

EncryptedPayload encrypt_message() {
    return std::get<EncryptedPayload>(encrypt_payload(capability, message, associated_data, 1024));
}

Bytes from_hex(const char *hex) {
    Bytes bytes(std::char_traits<char>::length(hex) / 2);
    std::size_t written = 0;
    const auto result =
        sodium_hex2bin(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size(), hex,
                       std::char_traits<char>::length(hex), nullptr, &written, nullptr);
    if (result != 0 || written != bytes.size()) {
        throw std::invalid_argument("invalid test vector");
    }
    return bytes;
}
} // namespace

TEST(SecurityIdentity, MatchesRfc8032TestVectorOne) {
    auto seed = from_hex("9d61b19deffd5a60ba844af492ec2cc4"
                         "4449c5697b326919703bac031cae7f60");
    const auto public_key = from_hex("d75a980182b10ab7d54bfed3c964073a"
                                     "0ee172f3daa62325af021a68f707511a");
    const auto signature = from_hex("e5564300c360ac729086e2cc806e828a"
                                    "84877f1eb8e5d974d873e06522490155"
                                    "5fb8821590a33bacc61e39701cf9b46b"
                                    "d25bf5f0595bbe24655141438e7a100b");
    auto imported = Identity::import_seed(seed);
    ASSERT_TRUE(std::holds_alternative<Identity>(imported));
    const auto &identity = std::get<Identity>(imported);
    const auto actual_key = identity.public_key();
    EXPECT_EQ(Bytes(actual_key.begin(), actual_key.end()), public_key);
    // RFC 8032 test 1 checks raw Ed25519, which only protocol code uses.
    const auto produced = std::get<Signature>(detail::ProtocolSigning::sign(identity, {}));
    EXPECT_EQ(Bytes(produced.begin(), produced.end()), signature);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        detail::verify_protocol_signature(public_key, {}, signature)));
    sodium_memzero(seed.data(), seed.size());
}

TEST(SecurityEncryption, DecryptsIndependentFixedVector) {
    const auto key = from_hex("000102030405060708090a0b0c0d0e0f"
                              "101112131415161718191a1b1c1d1e1f");
    const auto nonce = from_hex("000102030405060708090a0b0c0d0e0f1011121314151617");
    auto cipher = from_hex("8ddc7972a9f0054974a4ffeff332d8ad202223");
    auto result = decrypt_payload(key, nonce, cipher, associated_data, message.size());
    ASSERT_TRUE(std::holds_alternative<Bytes>(result));
    EXPECT_EQ(std::get<Bytes>(result), Bytes(message.begin(), message.end()));
    cipher[0] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(decrypt_payload(key, nonce, cipher, associated_data, 3)),
              CryptoError::AuthenticationFailed);
}

TEST(SecurityIdentity, GeneratesIndependentSigningIdentities) {
    static_assert(!std::is_copy_constructible_v<Identity>);
    static_assert(std::is_nothrow_move_constructible_v<Identity>);
    auto first = Identity::generate();
    auto second = Identity::generate();
    ASSERT_TRUE(std::holds_alternative<Identity>(first));
    ASSERT_TRUE(std::holds_alternative<Identity>(second));
    EXPECT_NE(std::get<Identity>(first).public_key(), std::get<Identity>(second).public_key());
    auto &identity = std::get<Identity>(first);
    const auto signature = std::get<Signature>(identity.sign(message));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        verify_signature(identity.public_key(), message, signature)));
    EXPECT_EQ(std::get<CryptoError>(
                  verify_signature(std::get<Identity>(second).public_key(), message, signature)),
              CryptoError::AuthenticationFailed);
}

TEST(SecurityIdentity, ImportsAndExportsSeedWithoutChangingIdentity) {
    auto identity = generate_identity();
    std::array<std::byte, identity_seed_size> seed{};
    ASSERT_TRUE(std::holds_alternative<std::monostate>(identity.export_seed(seed)));
    auto imported = Identity::import_seed(seed);
    ASSERT_TRUE(std::holds_alternative<Identity>(imported));
    auto &restored = std::get<Identity>(imported);
    EXPECT_EQ(restored.public_key(), identity.public_key());
    EXPECT_EQ(std::get<Signature>(restored.sign(message)),
              std::get<Signature>(identity.sign(message)));
    sodium_memzero(seed.data(), seed.size());
    EXPECT_EQ(std::get<CryptoError>(Identity::import_seed(Bytes{})), CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(Identity::import_seed(Bytes(identity_seed_size + 1))),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(identity.export_seed(std::span(seed).first(1))),
              CryptoError::InvalidLength);
}

TEST(SecurityIdentity, SignsEmptyMessageAndRejectsTampering) {
    auto identity = generate_identity();
    auto signature = std::get<Signature>(identity.sign({}));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        verify_signature(identity.public_key(), {}, signature)));
    EXPECT_EQ(std::get<CryptoError>(verify_signature(identity.public_key(), message, signature)),
              CryptoError::AuthenticationFailed);
    signature[0] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(verify_signature(identity.public_key(), {}, signature)),
              CryptoError::AuthenticationFailed);
    EXPECT_EQ(std::get<CryptoError>(verify_signature({}, {}, signature)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(
                  verify_signature(identity.public_key(), {}, ByteView(signature).first(1))),
              CryptoError::InvalidLength);
}

// Deliberately exercise the documented rejection contract for moved-from identities.
// NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
TEST(SecurityIdentity, RejectsPrivateOperationsAfterMove) {
    auto original = generate_identity();
    const auto public_key = original.public_key();
    auto moved = std::move(original);
    EXPECT_EQ(moved.public_key(), public_key);
    EXPECT_TRUE(std::holds_alternative<Signature>(moved.sign(message)));
    EXPECT_EQ(std::get<CryptoError>(original.sign(message)), CryptoError::InvalidIdentity);
    std::array<std::byte, identity_seed_size> seed{};
    EXPECT_EQ(std::get<CryptoError>(original.export_seed(seed)), CryptoError::InvalidIdentity);
    auto assigned = generate_identity();
    assigned = std::move(moved);
    EXPECT_EQ(assigned.public_key(), public_key);
    EXPECT_TRUE(std::holds_alternative<Signature>(assigned.sign(message)));
    EXPECT_EQ(std::get<CryptoError>(moved.sign(message)), CryptoError::InvalidIdentity);
}

// NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

TEST(SecurityEncryption, RoundTripsBinaryPayloadAndUsesFreshNonces) {
    const auto first = encrypt_message();
    const auto second = encrypt_message();
    EXPECT_NE(first.nonce, second.nonce);
    EXPECT_NE(first.ciphertext, second.ciphertext);
    EXPECT_EQ(first.ciphertext.size(), message.size() + encryption_tag_size);
    const auto decrypted =
        decrypt_payload(capability, first.nonce, first.ciphertext, associated_data, message.size());
    ASSERT_TRUE(std::holds_alternative<Bytes>(decrypted));
    EXPECT_EQ(std::get<Bytes>(decrypted), Bytes(message.begin(), message.end()));
}

TEST(SecurityEncryption, RoundTripsEmptyPayloadAndEmptyAssociatedData) {
    const auto encrypted = encrypt_payload(capability, {}, {}, 0);
    ASSERT_TRUE(std::holds_alternative<EncryptedPayload>(encrypted));
    const auto &payload = std::get<EncryptedPayload>(encrypted);
    EXPECT_EQ(payload.ciphertext.size(), encryption_tag_size);
    const auto decrypted = decrypt_payload(capability, payload.nonce, payload.ciphertext, {}, 0);
    ASSERT_TRUE(std::holds_alternative<Bytes>(decrypted));
    EXPECT_TRUE(std::get<Bytes>(decrypted).empty());
}

TEST(SecurityEncryption, RejectsWrongCapabilityAndAssociatedData) {
    const auto payload = encrypt_message();
    auto wrong_capability = capability;
    wrong_capability[1] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(decrypt_payload(wrong_capability, payload.nonce,
                                                    payload.ciphertext, associated_data, 1024)),
              CryptoError::AuthenticationFailed);
    EXPECT_EQ(std::get<CryptoError>(
                  decrypt_payload(capability, payload.nonce, payload.ciphertext, {}, 1024)),
              CryptoError::AuthenticationFailed);
}

TEST(SecurityEncryption, RejectsTamperedNonceCiphertextAndTag) {
    const auto payload = encrypt_message();
    auto nonce = payload.nonce;
    nonce[0] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(
                  decrypt_payload(capability, nonce, payload.ciphertext, associated_data, 1024)),
              CryptoError::AuthenticationFailed);
    for (const auto offset : {std::size_t{0}, payload.ciphertext.size() - 1}) {
        auto ciphertext = payload.ciphertext;
        ciphertext[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<CryptoError>(decrypt_payload(capability, payload.nonce, ciphertext,
                                                        associated_data, 1024)),
                  CryptoError::AuthenticationFailed);
    }
}

TEST(SecurityEncryption, RejectsMalformedLengthsAndResourceLimits) {
    const auto payload = encrypt_message();
    EXPECT_EQ(std::get<CryptoError>(encrypt_payload({}, message, associated_data, 1024)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(encrypt_payload(Bytes(capability_size + 1), {}, {}, 0)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(encrypt_payload(capability, message, associated_data, 2)),
              CryptoError::TooLarge);
    EXPECT_EQ(std::get<CryptoError>(
                  decrypt_payload({}, payload.nonce, payload.ciphertext, associated_data, 1024)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(
                  decrypt_payload(capability, {}, payload.ciphertext, associated_data, 1024)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(decrypt_payload(capability, Bytes(encryption_nonce_size + 1),
                                                    payload.ciphertext, associated_data, 1024)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(decrypt_payload(capability, payload.nonce,
                                                    Bytes(encryption_tag_size - 1), associated_data,
                                                    1024)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(decrypt_payload(capability, payload.nonce, payload.ciphertext,
                                                    associated_data, 2)),
              CryptoError::TooLarge);
}

TEST(SecuritySignatures, ApplicationSignaturesCannotBeUsedAsProtocolSignatures) {
    auto identity = std::get<Identity>(Identity::generate());
    const Bytes message{std::byte{'p'}, std::byte{'2'}, std::byte{'p'}};
    const auto application = std::get<Signature>(identity.sign(message));
    const auto protocol = std::get<Signature>(detail::ProtocolSigning::sign(identity, message));
    EXPECT_NE(application, protocol);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        verify_signature(identity.public_key(), message, application)));
    EXPECT_EQ(std::get<CryptoError>(detail::verify_protocol_signature(identity.public_key(),
                                                                       message, application)),
              CryptoError::AuthenticationFailed);
    EXPECT_EQ(std::get<CryptoError>(verify_signature(identity.public_key(), message, protocol)),
              CryptoError::AuthenticationFailed);
    const auto domain = std::as_bytes(std::span(application_signature_domain));
    Bytes prefixed(domain.begin(), domain.end());
    prefixed.insert(prefixed.end(), message.begin(), message.end());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        detail::verify_protocol_signature(identity.public_key(), prefixed, application)));
}
