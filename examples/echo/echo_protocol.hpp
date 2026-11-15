#pragma once

#include "p2pdc/frame.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace p2pdc_echo {
enum class Role { Initiator, Responder };
enum class EchoAction { SendEcho, SendAck, SendConfirmation, Complete, Invalid };
inline constexpr std::array echo_message{std::byte{0}, std::byte{1}, std::byte{255}};
inline constexpr std::array echo_ack{std::byte{0xa1}};
inline constexpr std::array echo_confirmation{std::byte{0xa2}};

class EchoProtocol {
  public:
    explicit EchoProtocol(Role role) : role_(role) {}
    [[nodiscard]] EchoAction receive(p2pdc::ByteView message) {
        if (role_ != Role::Initiator && role_ != Role::Responder)
            return EchoAction::Invalid;
        if (stage_ == 0 && matches(message, echo_message)) {
            ++stage_;
            return role_ == Role::Initiator ? EchoAction::SendAck : EchoAction::SendEcho;
        }
        if (stage_ == 1 && role_ == Role::Responder && matches(message, echo_ack)) {
            ++stage_;
            return EchoAction::SendConfirmation;
        }
        if (stage_ == 1 && role_ == Role::Initiator && matches(message, echo_confirmation)) {
            ++stage_;
            return EchoAction::Complete;
        }
        return EchoAction::Invalid;
    }

  private:
    template <std::size_t Size>
    [[nodiscard]] static bool matches(p2pdc::ByteView value,
                                      const std::array<std::byte, Size> &expected) {
        return value.size() == expected.size() &&
               std::equal(value.begin(), value.end(), expected.begin());
    }
    Role role_;
    std::uint8_t stage_ = 0;
};
} // namespace p2pdc_echo
