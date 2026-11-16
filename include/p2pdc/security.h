#pragma once

#include "p2pdc/frame.h"

#include <array>
#include <memory>
#include <string_view>

namespace p2pdc {
namespace detail {
struct ProtocolSigning;
}

inline constexpr std::size_t identity_seed_size = 32;
inline constexpr std::size_t signing_public_key_size = 32;
inline constexpr std::size_t signature_size = 64;
inline constexpr std::size_t capability_size = 32;
inline constexpr std::size_t encryption_nonce_size = 24;
inline constexpr std::size_t encryption_tag_size = 16;

using SigningPublicKey = std::array<std::byte, signing_public_key_size>;
using Signature = std::array<std::byte, signature_size>;
using EncryptionNonce = std::array<std::byte, encryption_nonce_size>;

// Prefix of every message signed or verified through the public API. No p2pdc
// protocol signature input starts with it, so an application that signs data
// with a node identity can never produce a valid protocol record.
inline constexpr std::string_view application_signature_domain = "p2pdc/application/signature/v1";

enum class CryptoError {
    InitializationFailed,
    AllocationFailed,
    InvalidLength,
    InvalidIdentity,
    AuthenticationFailed,
    TooLarge,
    OperationFailed
};

template <class T> using CryptoResult = std::variant<T, CryptoError>;
using CryptoStatus = CryptoResult<std::monostate>;

// Private keys use sodium_malloc/sodium_free (guarded, wiped, best-effort locked).
// Caller-owned import/export buffers must be protected and wiped by the caller.
// Signatures require callers to use canonical, domain-separated protocol messages.
class Identity {
  public:
    static CryptoResult<Identity> generate();
    static CryptoResult<Identity> import_seed(ByteView seed);
    Identity(Identity &&) noexcept;
    Identity &operator=(Identity &&) noexcept;
    ~Identity();
    Identity(const Identity &) = delete;
    Identity &operator=(const Identity &) = delete;

    [[nodiscard]] SigningPublicKey public_key() const;
    // Signs application_signature_domain || message.
    [[nodiscard]] CryptoResult<Signature> sign(ByteView message) const;
    [[nodiscard]] CryptoStatus export_seed(std::span<std::byte> destination) const;

  private:
    friend struct detail::ProtocolSigning;
    struct Secret;
    Identity(std::unique_ptr<Secret> secret, SigningPublicKey public_key);
    [[nodiscard]] CryptoResult<Signature> sign_raw(ByteView message) const;
    std::unique_ptr<Secret> secret_;
    SigningPublicKey public_key_;
};

// Verifies a signature made by Identity::sign over application_signature_domain || message.
[[nodiscard]] CryptoStatus verify_signature(ByteView public_key, ByteView message,
                                            ByteView signature);

struct EncryptedPayload {
    EncryptionNonce nonce;
    Bytes ciphertext; // Includes the authentication tag; no wire encoding implied.
};

// Capability must be 32 high-entropy secret bytes; length checks cannot ensure entropy.
// Uses domain-separated BLAKE2b derivation and XChaCha20-Poly1305 with random nonces.
// Associated data is authenticated, not encrypted. Callers own replay protection,
// encoding, freshness, peer authorization and per-purpose associated-data binding.
// Standard container allocation failures may throw std::bad_alloc or std::length_error.
[[nodiscard]] CryptoResult<EncryptedPayload> encrypt_payload(ByteView capability,
                                                             ByteView plaintext,
                                                             ByteView associated_data,
                                                             std::size_t max_plaintext_size);
[[nodiscard]] CryptoResult<Bytes> decrypt_payload(ByteView capability, ByteView nonce,
                                                  ByteView ciphertext, ByteView associated_data,
                                                  std::size_t max_plaintext_size);

} // namespace p2pdc
