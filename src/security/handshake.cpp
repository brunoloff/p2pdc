#include "p2pdc/handshake.h"

#include <sodium.h>

#include <algorithm>
#include <string_view>

namespace p2pdc {
namespace {
constexpr std::array header{std::byte{'P'}, std::byte{'D'}, std::byte{'H'}, std::byte{'S'},
                            std::byte{1}};
constexpr std::string_view sign_domain = "p2pdc/handshake/signature/v1";
constexpr std::string_view sdp_domain = "p2pdc/handshake/sdp/v1";
constexpr std::string_view group_domain = "p2pdc/handshake/group/v1";

bool nonzero(ByteView value) {
    return std::any_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte != std::byte{}; });
}
bool valid_role(HandshakeRole role) {
    return role == HandshakeRole::Offerer || role == HandshakeRole::Answerer;
}
bool valid_context(const HandshakeContext &ctx) {
    return nonzero(ctx.offerer_instance) && nonzero(ctx.answerer_instance) &&
           nonzero(ctx.attempt) && nonzero(ctx.group) && nonzero(ctx.offer_hash) &&
           nonzero(ctx.answer_hash);
}
bool valid_record(const HandshakeRecord &record) {
    return valid_role(record.role) && record.max_message_size > 0 && nonzero(record.nonce) &&
           ((record.kind == HandshakeKind::Hello && !nonzero(record.peer_nonce)) ||
            (record.kind == HandshakeKind::Confirm && nonzero(record.peer_nonce)));
}
void append(Bytes &output, ByteView input) {
    output.insert(output.end(), input.begin(), input.end());
}
void put_u32(Bytes &output, std::uint32_t value) {
    for (unsigned shift = 32; shift > 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> (shift - 8)) & 255U));
    }
}
Bytes signature_input(const HandshakeContext &ctx, ByteView body) {
    const auto domain = std::as_bytes(std::span(sign_domain));
    Bytes input(domain.begin(), domain.end());
    append(input, ctx.offerer_key);
    append(input, ctx.answerer_key);
    append(input, ctx.offerer_instance);
    append(input, ctx.answerer_instance);
    append(input, ctx.attempt);
    append(input, ctx.group);
    append(input, ctx.offer_hash);
    append(input, ctx.answer_hash);
    append(input, body);
    return input;
}
const SigningPublicKey &role_key(const HandshakeContext &ctx, HandshakeRole role) {
    return role == HandshakeRole::Offerer ? ctx.offerer_key : ctx.answerer_key;
}
Bytes encode_body(const HandshakeRecord &record) {
    Bytes body(header.begin(), header.end());
    body.reserve(handshake_record_size);
    body.push_back(static_cast<std::byte>(record.kind));
    body.push_back(static_cast<std::byte>(record.role));
    put_u32(body, record.max_message_size);
    append(body, record.nonce);
    append(body, record.peer_nonce);
    return body;
}
HandshakeRecord parse_body(ByteView wire) {
    HandshakeRecord record{};
    record.kind = static_cast<HandshakeKind>(wire[5]);
    record.role = static_cast<HandshakeRole>(wire[6]);
    for (const auto byte : wire.subspan(7, 4)) {
        record.max_message_size = (record.max_message_size << 8U) | std::to_integer<unsigned>(byte);
    }
    std::copy_n(wire.begin() + 11, 32, record.nonce.begin());
    std::copy_n(wire.begin() + 43, 32, record.peer_nonce.begin());
    return record;
}
} // namespace

CryptoResult<HandshakeHash> handshake_group(ByteView capability) {
    if (capability.size() != capability_size) {
        return CryptoError::InvalidLength;
    }
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    HandshakeHash result{};
    if (crypto_generichash(
            reinterpret_cast<unsigned char *>(result.data()), result.size(),
            reinterpret_cast<const unsigned char *>(group_domain.data()), group_domain.size(),
            reinterpret_cast<const unsigned char *>(capability.data()), capability.size()) != 0) {
        return CryptoError::OperationFailed;
    }
    return result;
}

CryptoResult<HandshakeHash> handshake_description_hash(ByteView sdp, std::size_t max_size) {
    if (sdp.empty() || sdp.size() > max_size) {
        return CryptoError::InvalidLength;
    }
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    crypto_generichash_state state;
    HandshakeHash result{};
    if (crypto_generichash_init(&state, nullptr, 0, result.size()) != 0 ||
        crypto_generichash_update(&state,
                                  reinterpret_cast<const unsigned char *>(sdp_domain.data()),
                                  sdp_domain.size()) != 0 ||
        crypto_generichash_update(&state, reinterpret_cast<const unsigned char *>(sdp.data()),
                                  sdp.size()) != 0 ||
        crypto_generichash_final(&state, reinterpret_cast<unsigned char *>(result.data()),
                                 result.size()) != 0) {
        return CryptoError::OperationFailed;
    }
    return result;
}

CryptoResult<HandshakeNonce> generate_handshake_nonce() {
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    HandshakeNonce nonce{};
    do {
        randombytes_buf(nonce.data(), nonce.size());
    } while (!nonzero(nonce));
    return nonce;
}

HandshakeResult<Bytes> encode_handshake(const Identity &identity, const HandshakeContext &context,
                                        const HandshakeRecord &record) {
    if (!valid_context(context)) {
        return HandshakeError::InvalidContext;
    }
    if (!valid_record(record)) {
        return HandshakeError::InvalidRecord;
    }
    if (identity.public_key() != role_key(context, record.role)) {
        return HandshakeError::WrongIdentity;
    }
    auto body = encode_body(record);
    auto signature = identity.sign(signature_input(context, body));
    if (auto *error = std::get_if<CryptoError>(&signature)) {
        return *error;
    }
    append(body, std::get<Signature>(signature));
    return body;
}

HandshakeResult<HandshakeRecord> decode_handshake(ByteView wire, const HandshakeContext &context,
                                                  HandshakeRole expected_role) {
    if (!valid_context(context)) {
        return HandshakeError::InvalidContext;
    }
    if (!valid_role(expected_role)) {
        return HandshakeError::WrongRole;
    }
    if (wire.size() != handshake_record_size ||
        !std::equal(header.begin(), header.end(), wire.begin())) {
        return HandshakeError::InvalidRecord;
    }
    auto record = parse_body(wire);
    if (!valid_record(record)) {
        return HandshakeError::InvalidRecord;
    }
    if (record.role != expected_role) {
        return HandshakeError::WrongRole;
    }
    const auto body = wire.first(handshake_record_size - signature_size);
    auto verified = verify_signature(role_key(context, expected_role),
                                     signature_input(context, body), wire.last(signature_size));
    if (auto *error = std::get_if<CryptoError>(&verified)) {
        return *error;
    }
    return record;
}

} // namespace p2pdc
