#pragma once

#include "echo_peer.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace p2pdc_echo {
using Environment = std::function<std::optional<std::string>(std::string_view)>;
struct StandaloneConfig {
    p2pdc::DiscoveryConfig discovery;
    p2pdc::DiscoveryCapability capability{};
    p2pdc::SigningPublicKey authorized_peer{};
    std::array<std::byte, 32> signing_seed{}, recipient_secret{};
    p2pdc::IceConfig ice;
    Role role = Role::Initiator;
};
[[nodiscard]] bool parse_hex(std::string_view text, std::span<std::byte> destination);
[[nodiscard]] std::variant<StandaloneConfig, std::string> parse_config(const Environment &read);
} // namespace p2pdc_echo
