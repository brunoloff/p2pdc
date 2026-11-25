#include "p2pdc/recipient_crypto.h"

#include <gtest/gtest.h>

#include <sodium.h>

#include <type_traits>

using namespace p2pdc;

namespace {
constexpr std::array<std::byte, recipient_box_secret_key_size> test_secret_key{std::byte{7}};
constexpr std::array message{std::byte{0}, std::byte{0xff}, std::byte{3}};

class RecipientCryptoTest : public ::testing::Test {
  protected:
    RecipientKeyPair recipient =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(test_secret_key));

    Bytes ciphertext() const {
        return std::get<Bytes>(seal_to_recipient(recipient.public_key(), message, message.size()));
    }
};
} // namespace

TEST_F(RecipientCryptoTest, GeneratedKeypairRoundTripsAllByteValues) {
    static_assert(!std::is_copy_constructible_v<RecipientKeyPair>);
    static_assert(std::is_nothrow_move_constructible_v<RecipientKeyPair>);
    auto generated = RecipientKeyPair::generate();
    ASSERT_TRUE(std::holds_alternative<RecipientKeyPair>(generated));
    auto &keypair = std::get<RecipientKeyPair>(generated);
    Bytes payload;
    for (unsigned int byte = 0; byte < 256; ++byte) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    const auto encrypted = seal_to_recipient(keypair.public_key(), payload, payload.size());
    ASSERT_TRUE(std::holds_alternative<Bytes>(encrypted));
    EXPECT_EQ(std::get<Bytes>(encrypted).size(), payload.size() + recipient_box_overhead);
    const auto decrypted = keypair.open_sealed(std::get<Bytes>(encrypted), payload.size());
    ASSERT_TRUE(std::holds_alternative<Bytes>(decrypted));
    EXPECT_EQ(std::get<Bytes>(decrypted), payload);
    EXPECT_EQ(std::get<CryptoError>(keypair.open_sealed(std::get<Bytes>(encrypted), 255)),
              CryptoError::TooLarge);
}

TEST_F(RecipientCryptoTest, RoundTripsEmptyPayload) {
    const auto encrypted = seal_to_recipient(recipient.public_key(), {}, 0);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encrypted));
    EXPECT_EQ(std::get<Bytes>(encrypted).size(), recipient_box_overhead);
    const auto decrypted = recipient.open_sealed(std::get<Bytes>(encrypted), 0);
    ASSERT_TRUE(std::holds_alternative<Bytes>(decrypted));
    EXPECT_TRUE(std::get<Bytes>(decrypted).empty());
    auto tampered = std::get<Bytes>(encrypted);
    tampered.back() ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(recipient.open_sealed(tampered, 0)),
              CryptoError::AuthenticationFailed);
}

TEST_F(RecipientCryptoTest, RejectsWrongRecipientAndCiphertextTampering) {
    const auto encrypted = ciphertext();
    const std::array<std::byte, recipient_box_secret_key_size> other_secret{std::byte{42}};
    auto other = std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(other_secret));
    EXPECT_EQ(std::get<CryptoError>(other.open_sealed(encrypted, message.size())),
              CryptoError::AuthenticationFailed);
    for (const auto offset :
         {std::size_t{0}, recipient_box_public_key_size, encrypted.size() - 1}) {
        auto tampered = encrypted;
        tampered[offset] ^= std::byte{1};
        EXPECT_EQ(std::get<CryptoError>(recipient.open_sealed(tampered, message.size())),
                  CryptoError::AuthenticationFailed);
    }
}

TEST_F(RecipientCryptoTest, ImportsExportsPrivateScalarAndPreservesPublicKey) {
    std::array<std::byte, recipient_box_secret_key_size> exported{};
    ASSERT_TRUE(std::holds_alternative<std::monostate>(recipient.export_secret_key(exported)));
    EXPECT_EQ(exported, test_secret_key);
    auto restored = RecipientKeyPair::import_secret_key(exported);
    sodium_memzero(exported.data(), exported.size());
    ASSERT_TRUE(std::holds_alternative<RecipientKeyPair>(restored));
    EXPECT_EQ(std::get<RecipientKeyPair>(restored).public_key(), recipient.public_key());
    const auto decrypted = std::get<RecipientKeyPair>(restored).open_sealed(ciphertext(), 3);
    ASSERT_TRUE(std::holds_alternative<Bytes>(decrypted));
    EXPECT_EQ(std::get<Bytes>(decrypted), Bytes(message.begin(), message.end()));
    EXPECT_EQ(std::get<CryptoError>(RecipientKeyPair::import_secret_key({})),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(
                  RecipientKeyPair::import_secret_key(Bytes(recipient_box_secret_key_size + 1))),
              CryptoError::InvalidLength);
}

TEST_F(RecipientCryptoTest, ExportRejectsWrongLengthsWithoutWriting) {
    std::array<std::byte, recipient_box_secret_key_size + 1> buffer{};
    const auto before = buffer;
    EXPECT_EQ(std::get<CryptoError>(recipient.export_secret_key(std::span(buffer).first(31))),
              CryptoError::InvalidLength);
    EXPECT_EQ(buffer, before);
    EXPECT_EQ(std::get<CryptoError>(recipient.export_secret_key(buffer)),
              CryptoError::InvalidLength);
    EXPECT_EQ(buffer, before);
}

TEST_F(RecipientCryptoTest, RejectsMalformedPublicKeysAndPlaintextLimit) {
    const auto public_key = recipient.public_key();
    for (std::size_t length = 0; length < recipient_box_public_key_size; ++length) {
        EXPECT_EQ(std::get<CryptoError>(seal_to_recipient(ByteView(public_key).first(length),
                                                          message, message.size())),
                  CryptoError::InvalidLength);
    }
    EXPECT_EQ(std::get<CryptoError>(seal_to_recipient(Bytes(recipient_box_public_key_size + 1),
                                                      message, message.size())),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(seal_to_recipient(public_key, message, message.size() - 1)),
              CryptoError::TooLarge);
    EXPECT_EQ(std::get<CryptoError>(seal_to_recipient(RecipientBoxPublicKey{}, message, 3)),
              CryptoError::OperationFailed);
}

TEST_F(RecipientCryptoTest, RejectsShortCiphertextAndOpenLimit) {
    for (std::size_t length = 0; length < recipient_box_overhead; ++length) {
        EXPECT_EQ(std::get<CryptoError>(recipient.open_sealed(Bytes(length), 3)),
                  CryptoError::InvalidLength);
    }
    EXPECT_EQ(std::get<CryptoError>(recipient.open_sealed(ciphertext(), message.size() - 1)),
              CryptoError::TooLarge);
    EXPECT_EQ(std::get<CryptoError>(recipient.open_sealed(Bytes(recipient_box_overhead), 0)),
              CryptoError::AuthenticationFailed);
}

// Deliberately test the documented rejection contract for moved-from keypairs.
// NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
TEST_F(RecipientCryptoTest, MovePreservesKeyAndInvalidatesPrivateOperations) {
    const auto public_key = recipient.public_key();
    const auto encrypted = ciphertext();
    auto moved = std::move(recipient);
    EXPECT_EQ(moved.public_key(), public_key);
    EXPECT_TRUE(std::holds_alternative<Bytes>(moved.open_sealed(encrypted, 3)));
    EXPECT_EQ(std::get<CryptoError>(recipient.open_sealed(encrypted, 3)),
              CryptoError::InvalidIdentity);
    std::array<std::byte, recipient_box_secret_key_size> exported{};
    EXPECT_EQ(std::get<CryptoError>(recipient.export_secret_key(exported)),
              CryptoError::InvalidIdentity);
    auto assigned = std::get<RecipientKeyPair>(RecipientKeyPair::generate());
    assigned = std::move(moved);
    EXPECT_EQ(assigned.public_key(), public_key);
    EXPECT_TRUE(std::holds_alternative<Bytes>(assigned.open_sealed(encrypted, 3)));
    EXPECT_EQ(std::get<CryptoError>(moved.open_sealed(encrypted, 3)), CryptoError::InvalidIdentity);
}
// NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

TEST_F(RecipientCryptoTest, ImportedSecretOutlivesCallerBufferAndReplacedKey) {
    auto restored = [&] {
        auto local_secret = test_secret_key;
        auto imported = RecipientKeyPair::import_secret_key(local_secret);
        sodium_memzero(local_secret.data(), local_secret.size());
        return std::get<RecipientKeyPair>(std::move(imported));
    }();
    const auto encrypted = ciphertext();
    recipient = std::get<RecipientKeyPair>(RecipientKeyPair::generate());
    const auto plaintext = restored.open_sealed(encrypted, 3);
    ASSERT_TRUE(std::holds_alternative<Bytes>(plaintext));
    EXPECT_EQ(std::get<Bytes>(plaintext), Bytes(message.begin(), message.end()));
}

TEST(SealToSigningKeyTest, IdentityOpensBoxesSealedToItsSigningKey) {
    auto identity = std::get<Identity>(Identity::generate());
    const auto public_key = identity.public_key();
    const auto sealed = seal_to_signing_key(public_key, message, message.size());
    ASSERT_TRUE(std::holds_alternative<Bytes>(sealed));
    auto derived = RecipientKeyPair::from_identity(identity);
    ASSERT_TRUE(std::holds_alternative<RecipientKeyPair>(derived));
    const auto opened =
        std::get<RecipientKeyPair>(derived).open_sealed(std::get<Bytes>(sealed), message.size());
    ASSERT_TRUE(std::holds_alternative<Bytes>(opened));
    EXPECT_EQ(std::get<Bytes>(opened), Bytes(message.begin(), message.end()));
}

TEST(SealToSigningKeyTest, AnotherIdentityCannotOpen) {
    auto identity = std::get<Identity>(Identity::generate());
    auto other = std::get<Identity>(Identity::generate());
    const auto sealed =
        std::get<Bytes>(seal_to_signing_key(identity.public_key(), message, message.size()));
    auto derived = std::get<RecipientKeyPair>(RecipientKeyPair::from_identity(other));
    EXPECT_EQ(std::get<CryptoError>(derived.open_sealed(sealed, message.size())),
              CryptoError::AuthenticationFailed);
}

TEST(SealToSigningKeyTest, RejectsMalformedSigningKeys) {
    const std::array<std::byte, 31> short_key{};
    EXPECT_EQ(std::get<CryptoError>(seal_to_signing_key(short_key, message, message.size())),
              CryptoError::InvalidLength);
    std::array<std::byte, signing_public_key_size> not_a_point{};
    not_a_point.fill(std::byte{0xff});
    EXPECT_EQ(std::get<CryptoError>(seal_to_signing_key(not_a_point, message, message.size())),
              CryptoError::InvalidIdentity);
}

TEST(SealToSigningKeyTest, MovedFromIdentityIsRefused) {
    auto identity = std::get<Identity>(Identity::generate());
    Identity moved = std::move(identity);
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is tested
    EXPECT_EQ(std::get<CryptoError>(RecipientKeyPair::from_identity(identity)),
              CryptoError::InvalidIdentity);
    EXPECT_TRUE(std::holds_alternative<RecipientKeyPair>(RecipientKeyPair::from_identity(moved)));
}
