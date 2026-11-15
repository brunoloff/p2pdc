#include "p2pdc/advertisement.h"

#include <sodium.h>

#include <algorithm>
#include <optional>

namespace p2pdc {
namespace {
constexpr std::array header{std::byte{'P'}, std::byte{'D'}, std::byte{'A'}, std::byte{'D'},
                            std::byte{1}};

std::optional<AdvertisementError> validate_times(const AdvertisementFields &fields,
                                                 AdvertisementPolicy policy) {
    if (policy.max_lifetime == 0) {
        return AdvertisementError::InvalidPolicy;
    }
    if (fields.expires_at <= fields.issued_at ||
        fields.expires_at - fields.issued_at > policy.max_lifetime) {
        return AdvertisementError::InvalidLifetime;
    }
    if (fields.issued_at > policy.now && fields.issued_at - policy.now > policy.clock_skew) {
        return AdvertisementError::NotYetValid;
    }
    if (policy.now >= fields.expires_at && policy.now - fields.expires_at >= policy.clock_skew) {
        return AdvertisementError::Expired;
    }
    return std::nullopt;
}

Bytes domain_bytes(std::string_view domain, ByteView bytes) {
    Bytes result;
    result.reserve(domain.size() + bytes.size());
    for (const auto value : domain) {
        result.push_back(static_cast<std::byte>(value));
    }
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}

struct GroupContext {
    std::array<unsigned char, 32> bytes{};
};

CryptoResult<GroupContext> derive_group_context(ByteView capability) {
    if (capability.size() != capability_size) {
        return CryptoError::InvalidLength;
    }
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    GroupContext context;
    if (crypto_generichash(
            context.bytes.data(), context.bytes.size(),
            reinterpret_cast<const unsigned char *>(advertisement_group_domain.data()),
            advertisement_group_domain.size(),
            reinterpret_cast<const unsigned char *>(capability.data()), capability.size()) != 0) {
        return CryptoError::OperationFailed;
    }
    return context;
}

Bytes signature_input(const GroupContext &context, ByteView body) {
    auto input =
        domain_bytes(advertisement_signature_domain, std::as_bytes(std::span(context.bytes)));
    input.insert(input.end(), body.begin(), body.end());
    return input;
}

void put_integer(Bytes &out, std::uint64_t value) {
    for (unsigned int shift = 64; shift > 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> (shift - 8)) & 0xffU));
    }
}

Bytes encode_body(const Identity &identity, const AdvertisementFields &fields) {
    Bytes body{std::byte{1}};
    body.reserve(advertisement_plaintext_size);
    const auto signing_key = identity.public_key();
    body.insert(body.end(), signing_key.begin(), signing_key.end());
    body.insert(body.end(), fields.recipient_public_key.begin(), fields.recipient_public_key.end());
    body.insert(body.end(), fields.instance_id.begin(), fields.instance_id.end());
    body.insert(body.end(), fields.signaling_inbox.begin(), fields.signaling_inbox.end());
    put_integer(body, fields.sequence);
    put_integer(body, fields.features);
    put_integer(body, fields.issued_at);
    put_integer(body, fields.expires_at);
    return body;
}

template <std::size_t N> std::array<std::byte, N> read_array(ByteView &bytes) {
    std::array<std::byte, N> result{};
    std::copy_n(bytes.begin(), N, result.begin());
    bytes = bytes.subspan(N);
    return result;
}

std::uint64_t read_integer(ByteView &bytes) {
    std::uint64_t value = 0;
    for (const auto byte : bytes.first(8)) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
    }
    bytes = bytes.subspan(8);
    return value;
}

Advertisement decode_body(ByteView body) {
    body = body.subspan(1);
    Advertisement result{};
    result.signing_public_key = read_array<signing_public_key_size>(body);
    result.fields.recipient_public_key = read_array<32>(body);
    result.fields.instance_id = read_array<16>(body);
    result.fields.signaling_inbox = read_array<20>(body);
    result.fields.sequence = read_integer(body);
    result.fields.features = read_integer(body);
    result.fields.issued_at = read_integer(body);
    result.fields.expires_at = read_integer(body);
    return result;
}

AdvertisementResult<Bytes> seal_body(ByteView capability, Bytes body) {
    auto encrypted =
        encrypt_payload(capability, body, domain_bytes(advertisement_aead_domain, header),
                        advertisement_plaintext_size);
    if (const auto *error = std::get_if<CryptoError>(&encrypted)) {
        return *error;
    }
    const auto &payload = std::get<EncryptedPayload>(encrypted);
    Bytes wire(header.begin(), header.end());
    wire.insert(wire.end(), payload.nonce.begin(), payload.nonce.end());
    wire.insert(wire.end(), payload.ciphertext.begin(), payload.ciphertext.end());
    return wire;
}

AdvertisementResult<Advertisement> verify_body(ByteView plaintext, const GroupContext &context,
                                               AdvertisementPolicy policy) {
    if (plaintext.size() != advertisement_plaintext_size) {
        return AdvertisementError::InvalidLength;
    }
    if (plaintext[0] != std::byte{1}) {
        return AdvertisementError::UnsupportedVersion;
    }
    const auto body = plaintext.first(advertisement_body_size);
    const auto input = signature_input(context, body);
    const auto verification = verify_signature(body.subspan(1, signing_public_key_size), input,
                                               plaintext.subspan(advertisement_body_size));
    if (const auto *error = std::get_if<CryptoError>(&verification)) {
        return *error;
    }
    const auto advertisement = decode_body(body);
    if (const auto error = validate_times(advertisement.fields, policy)) {
        return *error;
    }
    return advertisement;
}
} // namespace

AdvertisementResult<Bytes> encode_advertisement(const Identity &identity,
                                                const AdvertisementFields &fields,
                                                ByteView capability, AdvertisementPolicy policy) {
    if (const auto error = validate_times(fields, policy)) {
        return *error;
    }
    auto body = encode_body(identity, fields);
    const auto context = derive_group_context(capability);
    if (const auto *error = std::get_if<CryptoError>(&context)) {
        return *error;
    }
    const auto signed_body = identity.sign(signature_input(std::get<GroupContext>(context), body));
    if (const auto *error = std::get_if<CryptoError>(&signed_body)) {
        return *error;
    }
    const auto &signature = std::get<Signature>(signed_body);
    body.insert(body.end(), signature.begin(), signature.end());
    return seal_body(capability, std::move(body));
}

AdvertisementResult<Advertisement> decode_advertisement(ByteView wire, ByteView capability,
                                                        AdvertisementPolicy policy) {
    if (wire.size() != advertisement_wire_size) {
        return AdvertisementError::InvalidLength;
    }
    if (!std::equal(header.begin(), header.begin() + 4, wire.begin())) {
        return AdvertisementError::InvalidMagic;
    }
    if (wire[4] != std::byte{1}) {
        return AdvertisementError::UnsupportedVersion;
    }
    auto plaintext = decrypt_payload(
        capability, wire.subspan(advertisement_header_size, encryption_nonce_size),
        wire.subspan(advertisement_header_size + encryption_nonce_size),
        domain_bytes(advertisement_aead_domain, wire.first(advertisement_header_size)),
        advertisement_plaintext_size);
    if (const auto *error = std::get_if<CryptoError>(&plaintext)) {
        return *error;
    }
    const auto context = derive_group_context(capability);
    if (const auto *error = std::get_if<CryptoError>(&context)) {
        return *error;
    }
    return verify_body(std::get<Bytes>(plaintext), std::get<GroupContext>(context), policy);
}

} // namespace p2pdc
