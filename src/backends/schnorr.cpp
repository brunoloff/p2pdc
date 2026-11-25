#include "schnorr.hpp"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <sodium.h>

#include <memory>
#include <stdexcept>

namespace p2pdc::detail {
namespace {

struct ContextDeleter {
    void operator()(secp256k1_context *value) const { secp256k1_context_destroy(value); }
};

// Signing and verifying only read the context; one randomized context serves every thread.
const secp256k1_context *context() {
    static const auto value = [] {
        auto *created = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        if (!created)
            throw std::runtime_error("cannot create a secp256k1 context");
        std::array<unsigned char, 32> seed{};
        randombytes_buf(seed.data(), seed.size());
        if (!secp256k1_context_randomize(created, seed.data()))
            throw std::runtime_error("cannot randomize the secp256k1 context");
        sodium_memzero(seed.data(), seed.size());
        return std::unique_ptr<secp256k1_context, ContextDeleter>(created);
    }();
    return value.get();
}

struct Keypair {
    secp256k1_keypair value{};
    ~Keypair() { sodium_memzero(&value, sizeof value); }
};

} // namespace

std::optional<SchnorrPublicKey> schnorr_public_key(const SchnorrSecret &secret) {
    Keypair keypair;
    secp256k1_xonly_pubkey key{};
    SchnorrPublicKey result{};
    if (!secp256k1_keypair_create(context(), &keypair.value, secret.data()) ||
        !secp256k1_keypair_xonly_pub(context(), &key, nullptr, &keypair.value) ||
        !secp256k1_xonly_pubkey_serialize(context(), result.data(), &key))
        return std::nullopt;
    return result;
}

std::optional<SchnorrSignature> schnorr_sign(std::span<const unsigned char> message,
                                             const SchnorrSecret &secret,
                                             const std::array<unsigned char, 32> &aux) {
    Keypair keypair;
    if (!secp256k1_keypair_create(context(), &keypair.value, secret.data()))
        return std::nullopt;
    secp256k1_schnorrsig_extraparams extra = SECP256K1_SCHNORRSIG_EXTRAPARAMS_INIT;
    extra.ndata = const_cast<unsigned char *>(aux.data());
    SchnorrSignature signature{};
    if (!secp256k1_schnorrsig_sign_custom(context(), signature.data(), message.data(),
                                          message.size(), &keypair.value, &extra))
        return std::nullopt;
    return signature;
}

bool schnorr_verify(std::span<const unsigned char> message, const SchnorrPublicKey &key,
                    const SchnorrSignature &signature) {
    secp256k1_xonly_pubkey parsed{};
    return secp256k1_xonly_pubkey_parse(context(), &parsed, key.data()) &&
           secp256k1_schnorrsig_verify(context(), signature.data(), message.data(), message.size(),
                                       &parsed);
}

SchnorrSigner::SchnorrSigner() {
    for (;;) {
        randombytes_buf(secret_.data(), secret_.size());
        if (auto key = schnorr_public_key(secret_)) {
            public_key_ = *key;
            return;
        }
    }
}

SchnorrSignature SchnorrSigner::sign(std::span<const unsigned char> message) const {
    std::array<unsigned char, 32> aux{};
    randombytes_buf(aux.data(), aux.size());
    auto signature = schnorr_sign(message, secret_, aux);
    if (!signature)
        throw std::logic_error("a valid Schnorr key failed to sign");
    return *signature;
}

} // namespace p2pdc::detail
