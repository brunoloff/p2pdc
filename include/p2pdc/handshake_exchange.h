#pragma once

#include "p2pdc/handshake.h"

#include <memory>

#include <optional>

namespace p2pdc {

// Single serialized connection attempt. The exchange shares ownership of the identity.
// The caller must use a fresh unpredictable nonce for every connection attempt.
// send markers may be called only after the reliable transport accepted the
// corresponding encoded record. Any protocol error must terminate the connection.
class HandshakeExchange {
  public:
    HandshakeExchange(std::shared_ptr<const Identity> identity, HandshakeContext context,
                      HandshakeRole role, HandshakeNonce nonce, std::uint32_t receive_limit);
    HandshakeExchange(const HandshakeExchange &) = delete;
    HandshakeExchange &operator=(const HandshakeExchange &) = delete;
    HandshakeExchange(HandshakeExchange &&) = delete;
    HandshakeExchange &operator=(HandshakeExchange &&) = delete;
    [[nodiscard]] HandshakeResult<Bytes> hello() const;
    [[nodiscard]] HandshakeResult<std::monostate> mark_hello_sent();
    [[nodiscard]] HandshakeResult<Bytes> confirmation() const;
    [[nodiscard]] HandshakeResult<std::monostate> mark_confirmation_sent();
    [[nodiscard]] HandshakeResult<std::monostate> receive(ByteView wire);
    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::optional<std::uint32_t> message_limit() const;

  private:
    std::shared_ptr<const Identity> identity_;
    HandshakeContext context_;
    HandshakeRole role_;
    HandshakeNonce nonce_;
    std::uint32_t receive_limit_;
    std::optional<HandshakeRecord> peer_hello_;
    bool hello_sent_ = false;
    bool confirmation_sent_ = false;
    bool peer_confirmed_ = false;
};

} // namespace p2pdc
