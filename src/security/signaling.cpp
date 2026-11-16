#include "p2pdc/signaling.h"
#include "protocol_signature.hpp"

#include <sodium.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace p2pdc {
namespace {
constexpr std::array header{std::byte{'P'}, std::byte{'D'}, std::byte{'S'}, std::byte{'G'},
                            std::byte{1}};
struct GroupContext {
    std::array<unsigned char, 32> bytes{};
};

std::optional<SignalingError> validate_policy(SignalingPolicy policy) {
    if (policy.max_lifetime == 0 ||
        policy.max_payload_size > std::numeric_limits<std::uint32_t>::max() ||
        policy.max_payload_size >
            std::numeric_limits<std::size_t>::max() - signaling_wire_overhead) {
        return SignalingError::InvalidPolicy;
    }
    return std::nullopt;
}

std::optional<SignalingError> validate_fields(const SignalingFields &fields,
                                              SignalingPolicy policy) {
    if (fields.kind < SignalingKind::Offer || fields.kind > SignalingKind::Reject) {
        return SignalingError::InvalidKind;
    }
    if (fields.payload.size() > policy.max_payload_size) {
        return SignalingError::TooLarge;
    }
    if (fields.expires_at <= fields.issued_at ||
        fields.expires_at - fields.issued_at > policy.max_lifetime) {
        return SignalingError::InvalidLifetime;
    }
    if (fields.issued_at > policy.now && fields.issued_at - policy.now > policy.clock_skew) {
        return SignalingError::NotYetValid;
    }
    if (policy.now >= fields.expires_at && policy.now - fields.expires_at >= policy.clock_skew) {
        return SignalingError::Expired;
    }
    return std::nullopt;
}

CryptoResult<GroupContext> derive_group_context(ByteView capability) {
    if (capability.size() != capability_size) {
        return CryptoError::InvalidLength;
    }
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    GroupContext context;
    if (crypto_generichash(context.bytes.data(), context.bytes.size(),
                           reinterpret_cast<const unsigned char *>(signaling_group_domain.data()),
                           signaling_group_domain.size(),
                           reinterpret_cast<const unsigned char *>(capability.data()),
                           capability.size()) != 0) {
        return CryptoError::OperationFailed;
    }
    return context;
}

Bytes signature_input(const GroupContext &context, ByteView body) {
    const auto domain = std::as_bytes(std::span(signaling_signature_domain));
    Bytes result(domain.begin(), domain.end());
    const auto group = std::as_bytes(std::span(context.bytes));
    result.insert(result.end(), group.begin(), group.end());
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

void append(Bytes &bytes, ByteView value) { bytes.insert(bytes.end(), value.begin(), value.end()); }

template <unsigned int Size> void put_integer(Bytes &bytes, std::uint64_t value) {
    for (unsigned int shift = Size * 8; shift > 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> (shift - 8)) & 0xffU));
    }
}

Bytes encode_body(const Identity &sender, const SignalingFields &fields) {
    Bytes body(header.begin(), header.end());
    body.reserve(signaling_signed_overhead + fields.payload.size());
    body.push_back(static_cast<std::byte>(fields.kind));
    append(body, sender.public_key());
    append(body, fields.recipient.signing_key);
    append(body, fields.sender_instance);
    append(body, fields.recipient.instance);
    append(body, fields.attempt_id);
    put_integer<8>(body, fields.sequence);
    put_integer<8>(body, fields.issued_at);
    put_integer<8>(body, fields.expires_at);
    put_integer<4>(body, fields.payload.size());
    append(body, fields.payload);
    return body;
}

template <std::size_t N> std::array<std::byte, N> read_array(ByteView &bytes) {
    std::array<std::byte, N> result{};
    std::copy_n(bytes.begin(), N, result.begin());
    bytes = bytes.subspan(N);
    return result;
}

std::uint64_t read_integer(ByteView &bytes, unsigned int size) {
    std::uint64_t value = 0;
    for (const auto byte : bytes.first(size)) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
    }
    bytes = bytes.subspan(size);
    return value;
}

SignalingEnvelope decode_body(ByteView body) {
    body = body.subspan(signaling_header_size);
    SignalingEnvelope result{};
    result.fields.kind = static_cast<SignalingKind>(read_integer(body, 1));
    result.sender_signing_key = read_array<32>(body);
    result.fields.recipient.signing_key = read_array<32>(body);
    result.fields.sender_instance = read_array<16>(body);
    result.fields.recipient.instance = read_array<16>(body);
    result.fields.attempt_id = read_array<16>(body);
    result.fields.sequence = read_integer(body, 8);
    result.fields.issued_at = read_integer(body, 8);
    result.fields.expires_at = read_integer(body, 8);
    static_cast<void>(read_integer(body, 4)); // Length checked before signature verification.
    result.fields.payload.assign(body.begin(), body.end());
    return result;
}

std::optional<SignalingError> validate_header(ByteView bytes) {
    if (!std::equal(header.begin(), header.begin() + 4, bytes.begin())) {
        return SignalingError::InvalidMagic;
    }
    if (bytes[4] != std::byte{1}) {
        return SignalingError::UnsupportedVersion;
    }
    return std::nullopt;
}

std::optional<SignalingError> validate_plaintext(ByteView plaintext) {
    if (plaintext.size() < signaling_signed_overhead) {
        return SignalingError::InvalidLength;
    }
    if (const auto error = validate_header(plaintext)) {
        return error;
    }
    auto length = plaintext.subspan(signaling_body_header_size - 4, 4);
    if (read_integer(length, 4) != plaintext.size() - signaling_signed_overhead) {
        return SignalingError::InvalidLength;
    }
    return std::nullopt;
}

SignalingResult<SignalingEnvelope> verify_plaintext(ByteView plaintext, const GroupContext &context,
                                                    const SignalingRecipient &expected_recipient,
                                                    SignalingPolicy policy) {
    if (const auto error = validate_plaintext(plaintext)) {
        return *error;
    }
    const auto body = plaintext.first(plaintext.size() - signature_size);
    const auto verified = detail::verify_protocol_signature(
        body.subspan(6, signing_public_key_size), signature_input(context, body),
        plaintext.last(signature_size));
    if (const auto *error = std::get_if<CryptoError>(&verified)) {
        return *error;
    }
    auto envelope = decode_body(body);
    if (envelope.fields.recipient != expected_recipient) {
        return SignalingError::RecipientMismatch;
    }
    if (const auto error = validate_fields(envelope.fields, policy)) {
        return *error;
    }
    return envelope;
}

SignalingResult<Bytes> seal_body(ByteView body, const RecipientBoxPublicKey &recipient_key) {
    auto sealed = seal_to_recipient(recipient_key, body, body.size());
    if (const auto *error = std::get_if<CryptoError>(&sealed)) {
        return *error;
    }
    Bytes wire(header.begin(), header.end());
    append(wire, std::get<Bytes>(sealed));
    return wire;
}
} // namespace

SignalingResult<Bytes> encode_signaling(const Identity &sender, const SignalingFields &fields,
                                        const RecipientBoxPublicKey &recipient_encryption_key,
                                        ByteView capability, SignalingPolicy policy) {
    if (const auto error = validate_policy(policy)) {
        return *error;
    }
    if (const auto error = validate_fields(fields, policy)) {
        return *error;
    }
    const auto context = derive_group_context(capability);
    if (const auto *error = std::get_if<CryptoError>(&context)) {
        return *error;
    }
    auto body = encode_body(sender, fields);
    const auto signature = detail::ProtocolSigning::sign(
        sender, signature_input(std::get<GroupContext>(context), body));
    if (const auto *error = std::get_if<CryptoError>(&signature)) {
        return *error;
    }
    append(body, std::get<Signature>(signature));
    return seal_body(body, recipient_encryption_key);
}

SignalingResult<SignalingEnvelope> decode_signaling(ByteView wire,
                                                    const RecipientKeyPair &recipient_key,
                                                    const SignalingRecipient &expected_recipient,
                                                    ByteView capability, SignalingPolicy policy) {
    if (const auto error = validate_policy(policy)) {
        return *error;
    }
    if (wire.size() < signaling_wire_overhead) {
        return SignalingError::InvalidLength;
    }
    if (wire.size() - signaling_wire_overhead > policy.max_payload_size) {
        return SignalingError::TooLarge;
    }
    if (const auto error = validate_header(wire)) {
        return *error;
    }
    const auto context = derive_group_context(capability);
    if (const auto *error = std::get_if<CryptoError>(&context)) {
        return *error;
    }
    const auto plaintext = recipient_key.open_sealed(
        wire.subspan(signaling_header_size), signaling_signed_overhead + policy.max_payload_size);
    if (const auto *error = std::get_if<CryptoError>(&plaintext)) {
        return *error;
    }
    return verify_plaintext(std::get<Bytes>(plaintext), std::get<GroupContext>(context),
                            expected_recipient, policy);
}

} // namespace p2pdc
