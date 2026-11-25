#pragma once

#include "p2pdc/security.h"

namespace p2pdc {

inline constexpr std::size_t recipient_box_public_key_size = 32;
inline constexpr std::size_t recipient_box_secret_key_size = 32;
inline constexpr std::size_t recipient_box_overhead = 48;
using RecipientBoxPublicKey = std::array<std::byte, recipient_box_public_key_size>;

// X25519 keypair for libsodium sealed boxes (X25519/XSalsa20-Poly1305).
// Secret keys use guarded/wiped sodium_malloc storage; memory locking is best effort.
// Import/export uses raw 32-byte private scalars, not Ed25519 seeds or identities.
// Caller must protect and wipe its own import/export buffers. Moved-from keypairs
// retain public information but reject private-key operations with InvalidIdentity.
class RecipientKeyPair {
  public:
    static CryptoResult<RecipientKeyPair> generate();
    static CryptoResult<RecipientKeyPair> import_secret_key(ByteView secret_key);
    // The X25519 keypair of an Ed25519 identity (libsodium's
    // crypto_sign_ed25519_sk_to_curve25519), which opens boxes sealed with
    // seal_to_signing_key to that identity's public key. Signing and sealing then share one
    // key; libsodium supports this conversion for exactly that use.
    static CryptoResult<RecipientKeyPair> from_identity(const Identity &identity);
    RecipientKeyPair(RecipientKeyPair &&) noexcept;
    RecipientKeyPair &operator=(RecipientKeyPair &&) noexcept;
    ~RecipientKeyPair();
    RecipientKeyPair(const RecipientKeyPair &) = delete;
    RecipientKeyPair &operator=(const RecipientKeyPair &) = delete;

    [[nodiscard]] RecipientBoxPublicKey public_key() const;
    [[nodiscard]] CryptoStatus export_secret_key(std::span<std::byte> destination) const;
    [[nodiscard]] CryptoResult<Bytes> open_sealed(ByteView ciphertext,
                                                  std::size_t max_plaintext_size) const;

  private:
    struct Secret;
    RecipientKeyPair(std::unique_ptr<Secret> secret, RecipientBoxPublicKey public_key);
    std::unique_ptr<Secret> secret_;
    RecipientBoxPublicKey public_key_;
};

// Anonymous sealed boxes provide recipient confidentiality and tamper detection,
// not sender authentication. Signed, capability-bound protocol envelopes remain
// required. No replay, freshness, authorization or associated-data API is implied.
// Output is libsodium's sealed-box encoding (ephemeral key || encrypted message).
// Standard container allocations can throw std::bad_alloc or std::length_error.
[[nodiscard]] CryptoResult<Bytes> seal_to_recipient(ByteView recipient_public_key,
                                                    ByteView plaintext,
                                                    std::size_t max_plaintext_size);

// Seals to the X25519 key of an Ed25519 signing public key
// (crypto_sign_ed25519_pk_to_curve25519); RecipientKeyPair::from_identity opens it.
// InvalidIdentity when the key is not a valid Ed25519 point.
[[nodiscard]] CryptoResult<Bytes> seal_to_signing_key(ByteView signing_public_key,
                                                      ByteView plaintext,
                                                      std::size_t max_plaintext_size);

} // namespace p2pdc
