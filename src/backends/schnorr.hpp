#pragma once

#include <array>
#include <optional>
#include <span>

namespace p2pdc::detail {

using SchnorrSecret = std::array<unsigned char, 32>;
using SchnorrPublicKey = std::array<unsigned char, 32>; // x-only (BIP340)
using SchnorrSignature = std::array<unsigned char, 64>;

// BIP340 Schnorr signatures over secp256k1 (libsecp256k1), as Nostr relays require of events.
// Only throwaway keys sign: p2pdc's own signatures inside each value are the ones peers check.

// Nullopt for a secret that is not a valid secp256k1 secret key.
[[nodiscard]] std::optional<SchnorrPublicKey> schnorr_public_key(const SchnorrSecret &secret);
// Signs message with auxiliary randomness aux (BIP340 "aux_rand"); nullopt for an invalid
// secret.
[[nodiscard]] std::optional<SchnorrSignature>
schnorr_sign(std::span<const unsigned char> message, const SchnorrSecret &secret,
             const std::array<unsigned char, 32> &aux);
[[nodiscard]] bool schnorr_verify(std::span<const unsigned char> message,
                                  const SchnorrPublicKey &key, const SchnorrSignature &signature);

// A fresh random key that signs with fresh randomness each time.
class SchnorrSigner {
  public:
    SchnorrSigner();
    [[nodiscard]] const SchnorrPublicKey &public_key() const { return public_key_; }
    [[nodiscard]] SchnorrSignature sign(std::span<const unsigned char> message) const;

  private:
    SchnorrSecret secret_{};
    SchnorrPublicKey public_key_{};
};

} // namespace p2pdc::detail
