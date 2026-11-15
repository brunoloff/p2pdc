#include "p2pdc/recipient_crypto.h"

#include <sodium.h>

#include <cstring>
#include <limits>
#include <new>

namespace p2pdc {
namespace {
static_assert(recipient_box_public_key_size == crypto_box_PUBLICKEYBYTES);
static_assert(recipient_box_secret_key_size == crypto_box_SECRETKEYBYTES);
static_assert(recipient_box_overhead == crypto_box_SEALBYTES);

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

bool valid_plaintext_size(std::size_t size, std::size_t limit) {
    return size <= limit && size <= crypto_box_MESSAGEBYTES_MAX &&
           size <= std::numeric_limits<std::size_t>::max() - recipient_box_overhead;
}
} // namespace

struct RecipientKeyPair::Secret {
    unsigned char *bytes = static_cast<unsigned char *>(sodium_malloc(crypto_box_SECRETKEYBYTES));
    ~Secret() { sodium_free(bytes); }
};

RecipientKeyPair::RecipientKeyPair(std::unique_ptr<Secret> secret, RecipientBoxPublicKey public_key)
    : secret_(std::move(secret)), public_key_(public_key) {}
RecipientKeyPair::RecipientKeyPair(RecipientKeyPair &&) noexcept = default;
RecipientKeyPair &RecipientKeyPair::operator=(RecipientKeyPair &&) noexcept = default;
RecipientKeyPair::~RecipientKeyPair() = default;

CryptoResult<RecipientKeyPair> RecipientKeyPair::generate() {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    auto secret = std::unique_ptr<Secret>(new (std::nothrow) Secret);
    if (!secret || !secret->bytes) {
        return CryptoError::AllocationFailed;
    }
    RecipientBoxPublicKey public_key{};
    if (crypto_box_keypair(mutable_data(std::span(public_key)), secret->bytes) != 0) {
        return CryptoError::OperationFailed;
    }
    return RecipientKeyPair(std::move(secret), public_key);
}

CryptoResult<RecipientKeyPair> RecipientKeyPair::import_secret_key(ByteView secret_key) {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    if (secret_key.size() != recipient_box_secret_key_size) {
        return CryptoError::InvalidLength;
    }
    auto secret = std::unique_ptr<Secret>(new (std::nothrow) Secret);
    if (!secret || !secret->bytes) {
        return CryptoError::AllocationFailed;
    }
    std::memcpy(secret->bytes, secret_key.data(), secret_key.size());
    RecipientBoxPublicKey public_key{};
    if (crypto_scalarmult_base(mutable_data(std::span(public_key)), secret->bytes) != 0) {
        return CryptoError::OperationFailed;
    }
    return RecipientKeyPair(std::move(secret), public_key);
}

RecipientBoxPublicKey RecipientKeyPair::public_key() const { return public_key_; }

CryptoStatus RecipientKeyPair::export_secret_key(std::span<std::byte> destination) const {
    if (!secret_) {
        return CryptoError::InvalidIdentity;
    }
    if (destination.size() != recipient_box_secret_key_size) {
        return CryptoError::InvalidLength;
    }
    std::memcpy(destination.data(), secret_->bytes, destination.size());
    return std::monostate{};
}

CryptoResult<Bytes> RecipientKeyPair::open_sealed(ByteView ciphertext,
                                                  std::size_t max_plaintext_size) const {
    if (!secret_) {
        return CryptoError::InvalidIdentity;
    }
    if (ciphertext.size() < recipient_box_overhead) {
        return CryptoError::InvalidLength;
    }
    const auto size = ciphertext.size() - recipient_box_overhead;
    if (!valid_plaintext_size(size, max_plaintext_size)) {
        return CryptoError::TooLarge;
    }
    Bytes plaintext(size);
    unsigned char empty = 0;
    auto *output = plaintext.empty() ? &empty : mutable_data(std::span(plaintext));
    if (crypto_box_seal_open(output, sodium_data(ciphertext), ciphertext.size(),
                             sodium_data(public_key_), secret_->bytes) != 0) {
        sodium_memzero(output, plaintext.size());
        return CryptoError::AuthenticationFailed;
    }
    return plaintext;
}

// Byte spans intentionally accept untrusted key lengths for entry-point validation.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
CryptoResult<Bytes> seal_to_recipient(ByteView recipient_public_key, ByteView plaintext,
                                      std::size_t max_plaintext_size) {
    if (!initialized()) {
        return CryptoError::InitializationFailed;
    }
    if (recipient_public_key.size() != recipient_box_public_key_size) {
        return CryptoError::InvalidLength;
    }
    if (!valid_plaintext_size(plaintext.size(), max_plaintext_size)) {
        return CryptoError::TooLarge;
    }
    Bytes ciphertext(plaintext.size() + recipient_box_overhead);
    if (crypto_box_seal(mutable_data(std::span(ciphertext)), sodium_data(plaintext),
                        plaintext.size(), sodium_data(recipient_public_key)) != 0) {
        return CryptoError::OperationFailed;
    }
    return ciphertext;
}

} // namespace p2pdc
