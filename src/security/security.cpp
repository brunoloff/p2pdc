#include "p2pdc/security.h"

#include <sodium.h>

#include <limits>
#include <new>

namespace p2pdc {
namespace {
static_assert(identity_seed_size == crypto_sign_SEEDBYTES);
static_assert(signing_public_key_size == crypto_sign_PUBLICKEYBYTES);
static_assert(signature_size == crypto_sign_BYTES);
static_assert(encryption_nonce_size == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(encryption_tag_size == crypto_aead_xchacha20poly1305_ietf_ABYTES);

bool initialized() {
    static const bool ready = sodium_init() >= 0;
    return ready;
}

const unsigned char *sodium_data(ByteView bytes) {
    static constexpr unsigned char empty = 0;
    return bytes.empty() ? &empty : reinterpret_cast<const unsigned char *>(bytes.data());
}

unsigned char *mutable_data(std::span<std::byte> bytes) {
    return reinterpret_cast<unsigned char *>(bytes.data());
}

struct DerivedKey {
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> bytes{};
    ~DerivedKey() { sodium_memzero(bytes.data(), bytes.size()); }
};

CryptoStatus derive_key(ByteView capability, DerivedKey &key) {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    if (capability.size() != capability_size) {
        return CryptoError::InvalidLength;
    }
    constexpr unsigned char context[] = "p2pdc/capability-encryption/key/v1";
    if (crypto_generichash(key.bytes.data(), key.bytes.size(), context, sizeof(context) - 1,
                           sodium_data(capability), capability.size()) != 0) {
        return CryptoError::OperationFailed;
    }
    return std::monostate{};
}

bool valid_plaintext_size(std::size_t size, std::size_t limit) {
    return size <= limit && size <= crypto_aead_xchacha20poly1305_ietf_MESSAGEBYTES_MAX &&
           size <= std::numeric_limits<std::size_t>::max() - encryption_tag_size;
}
} // namespace

struct Identity::Secret {
    unsigned char *bytes = static_cast<unsigned char *>(sodium_malloc(crypto_sign_SECRETKEYBYTES));
    ~Secret() { sodium_free(bytes); }
};

Identity::Identity(std::unique_ptr<Secret> secret, SigningPublicKey public_key)
    : secret_(std::move(secret)), public_key_(public_key) {}
Identity::Identity(Identity &&) noexcept = default;
Identity &Identity::operator=(Identity &&) noexcept = default;
Identity::~Identity() = default;

CryptoResult<Identity> Identity::generate() {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    auto secret = std::unique_ptr<Secret>(new (std::nothrow) Secret);
    if (!secret || !secret->bytes) {
        return CryptoError::AllocationFailed;
    }
    SigningPublicKey public_key{};
    if (crypto_sign_keypair(mutable_data(std::span(public_key)), secret->bytes) != 0) {
        return CryptoError::OperationFailed;
    }
    return Identity(std::move(secret), public_key);
}

CryptoResult<Identity> Identity::import_seed(ByteView seed) {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    if (seed.size() != identity_seed_size) {
        return CryptoError::InvalidLength;
    }
    auto secret = std::unique_ptr<Secret>(new (std::nothrow) Secret);
    if (!secret || !secret->bytes) {
        return CryptoError::AllocationFailed;
    }
    SigningPublicKey public_key{};
    if (crypto_sign_seed_keypair(mutable_data(std::span(public_key)), secret->bytes,
                                 sodium_data(seed)) != 0) {
        return CryptoError::OperationFailed;
    }
    return Identity(std::move(secret), public_key);
}

SigningPublicKey Identity::public_key() const { return public_key_; }

CryptoResult<Signature> Identity::sign(ByteView message) const {
    if (!secret_) {
        return CryptoError::InvalidIdentity;
    }
    if (message.size() > crypto_sign_MESSAGEBYTES_MAX) {
        return CryptoError::TooLarge;
    }
    Signature signature{};
    if (crypto_sign_detached(mutable_data(std::span(signature)), nullptr, sodium_data(message),
                             message.size(), secret_->bytes) != 0) {
        return CryptoError::OperationFailed;
    }
    return signature;
}

CryptoStatus Identity::export_seed(std::span<std::byte> destination) const {
    if (!secret_) {
        return CryptoError::InvalidIdentity;
    }
    if (destination.size() != identity_seed_size) {
        return CryptoError::InvalidLength;
    }
    if (crypto_sign_ed25519_sk_to_seed(mutable_data(destination), secret_->bytes) != 0) {
        return CryptoError::OperationFailed;
    }
    return std::monostate{};
}

CryptoStatus verify_signature(ByteView public_key, ByteView message, ByteView signature) {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    if (public_key.size() != signing_public_key_size || signature.size() != signature_size) {
        return CryptoError::InvalidLength;
    }
    if (message.size() > crypto_sign_MESSAGEBYTES_MAX) {
        return CryptoError::TooLarge;
    }
    if (crypto_sign_verify_detached(sodium_data(signature), sodium_data(message), message.size(),
                                    sodium_data(public_key)) != 0) {
        return CryptoError::AuthenticationFailed;
    }
    return std::monostate{};
}

// Byte spans intentionally allow malformed untrusted lengths to be rejected at entry.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
CryptoResult<EncryptedPayload> encrypt_payload(ByteView capability, ByteView plaintext,
                                               ByteView associated_data,
                                               std::size_t max_plaintext_size) {
    DerivedKey key;
    const auto derived = derive_key(capability, key);
    if (const auto *error = std::get_if<CryptoError>(&derived)) {
        return *error;
    }
    if (!valid_plaintext_size(plaintext.size(), max_plaintext_size)) {
        return CryptoError::TooLarge;
    }
    EncryptedPayload result{};
    result.ciphertext.resize(plaintext.size() + encryption_tag_size);
    randombytes_buf(result.nonce.data(), result.nonce.size());
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            mutable_data(std::span(result.ciphertext)), nullptr, sodium_data(plaintext),
            plaintext.size(), sodium_data(associated_data), associated_data.size(), nullptr,
            sodium_data(ByteView(result.nonce)), key.bytes.data()) != 0) {
        return CryptoError::OperationFailed;
    }
    return result;
}

CryptoResult<Bytes> decrypt_payload(ByteView capability, ByteView nonce, ByteView ciphertext,
                                    ByteView associated_data, std::size_t max_plaintext_size) {
    DerivedKey key;
    const auto derived = derive_key(capability, key);
    if (const auto *error = std::get_if<CryptoError>(&derived)) {
        return *error;
    }
    if (nonce.size() != encryption_nonce_size || ciphertext.size() < encryption_tag_size) {
        return CryptoError::InvalidLength;
    }
    const auto size = ciphertext.size() - encryption_tag_size;
    if (!valid_plaintext_size(size, max_plaintext_size)) {
        return CryptoError::TooLarge;
    }
    Bytes plaintext(size);
    unsigned char empty = 0;
    auto *output = plaintext.empty() ? &empty : mutable_data(std::span(plaintext));
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            output, nullptr, nullptr, sodium_data(ciphertext), ciphertext.size(),
            sodium_data(associated_data), associated_data.size(), sodium_data(nonce),
            key.bytes.data()) != 0) {
        sodium_memzero(output, plaintext.size());
        return CryptoError::AuthenticationFailed;
    }
    return plaintext;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace p2pdc
