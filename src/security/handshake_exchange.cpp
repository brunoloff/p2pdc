#include "p2pdc/handshake_exchange.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2pdc {

HandshakeExchange::HandshakeExchange(const Identity &identity, HandshakeContext context,
                                     HandshakeRole role, HandshakeNonce nonce,
                                     std::uint32_t receive_limit)
    : identity_(identity), context_(std::move(context)), role_(role), nonce_(nonce),
      receive_limit_(receive_limit) {
    auto encoded = hello();
    if (!std::holds_alternative<Bytes>(encoded)) {
        throw std::invalid_argument("invalid handshake identity, context, role, nonce or limit");
    }
}

HandshakeResult<Bytes> HandshakeExchange::hello() const {
    if (hello_sent_) {
        return HandshakeError::InvalidState;
    }
    return encode_handshake(identity_, context_,
                            {HandshakeKind::Hello, role_, receive_limit_, nonce_, {}});
}

HandshakeResult<std::monostate> HandshakeExchange::mark_hello_sent() {
    if (hello_sent_) {
        return HandshakeError::InvalidState;
    }
    hello_sent_ = true;
    return std::monostate{};
}

HandshakeResult<Bytes> HandshakeExchange::confirmation() const {
    if (!hello_sent_ || !peer_hello_ || confirmation_sent_) {
        return HandshakeError::InvalidState;
    }
    return encode_handshake(
        identity_, context_,
        {HandshakeKind::Confirm, role_, receive_limit_, nonce_, peer_hello_->nonce});
}

HandshakeResult<std::monostate> HandshakeExchange::mark_confirmation_sent() {
    if (!hello_sent_ || !peer_hello_ || confirmation_sent_) {
        return HandshakeError::InvalidState;
    }
    confirmation_sent_ = true;
    return std::monostate{};
}

HandshakeResult<std::monostate> HandshakeExchange::receive(ByteView wire) {
    const auto expected =
        role_ == HandshakeRole::Offerer ? HandshakeRole::Answerer : HandshakeRole::Offerer;
    auto decoded = decode_handshake(wire, context_, expected);
    if (auto *error = std::get_if<HandshakeError>(&decoded)) {
        return *error;
    }
    if (auto *error = std::get_if<CryptoError>(&decoded)) {
        return *error;
    }
    const auto &record = std::get<HandshakeRecord>(decoded);
    if (record.kind == HandshakeKind::Hello && !peer_hello_ && record.nonce != nonce_) {
        peer_hello_ = record;
        return std::monostate{};
    }
    if (record.kind == HandshakeKind::Confirm && hello_sent_ && peer_hello_ && !peer_confirmed_ &&
        record.nonce == peer_hello_->nonce && record.peer_nonce == nonce_ &&
        record.max_message_size == peer_hello_->max_message_size) {
        peer_confirmed_ = true;
        return std::monostate{};
    }
    return HandshakeError::InvalidState;
}

bool HandshakeExchange::ready() const { return confirmation_sent_ && peer_confirmed_; }
std::optional<std::uint32_t> HandshakeExchange::message_limit() const {
    if (!ready()) {
        return std::nullopt;
    }
    return std::min(receive_limit_, peer_hello_->max_message_size);
}

} // namespace p2pdc
