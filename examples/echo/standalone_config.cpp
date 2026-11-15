#include "standalone_config.hpp"

#include <charconv>

namespace p2pdc_echo {
using namespace p2pdc;
namespace {
int hex_digit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}
bool nonzero(std::span<const std::byte> value) {
    for (const auto byte : value)
        if (byte != std::byte{})
            return true;
    return false;
}
bool read_hex(const Environment &read, std::string_view name, std::span<std::byte> destination) {
    auto value = read(name);
    return value && parse_hex(*value, destination) && nonzero(destination);
}
bool parse_number(std::string_view text, std::uint32_t maximum, std::uint32_t &out) {
    if (text.empty())
        return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out <= maximum;
}
bool port(const Environment &read, std::string_view name, std::uint16_t fallback,
          std::uint16_t &out) {
    auto value = read(name);
    if (!value) {
        out = fallback;
        return true;
    }
    std::uint32_t number = 0;
    if (!parse_number(*value, 65535, number) || !number)
        return false;
    out = static_cast<std::uint16_t>(number);
    return true;
}
std::optional<std::string> discovery(const Environment &read, StandaloneConfig &out) {
    auto host = read("P2PDC_ECHO_BOOTSTRAP_HOST");
    auto proxy = read("P2PDC_ECHO_PROXY");
    if ((!host || host->empty()) && (!proxy || proxy->empty()))
        return "set a bootstrap host or browser proxy";
    if (host && !host->empty()) {
        std::uint16_t number = 0;
        if (!port(read, "P2PDC_ECHO_BOOTSTRAP_PORT", 4222, number))
            return "invalid bootstrap port";
        out.discovery.bootstrap_servers.push_back({*host, number});
    }
    if (proxy && !proxy->empty())
        out.discovery.browser_proxy_urls.push_back(*proxy);
    if (auto network = read("P2PDC_ECHO_NETWORK");
        network && !parse_number(*network, UINT32_MAX, out.discovery.network_id))
        return "invalid network ID";
    return {};
}
std::optional<std::string> stun(const Environment &read, StandaloneConfig &out) {
    auto host = read("P2PDC_ECHO_STUN_HOST");
    if (!host || host->empty())
        return read("P2PDC_ECHO_STUN_PORT") ? std::optional<std::string>("STUN port needs a host")
                                            : std::nullopt;
    std::uint16_t number = 0;
    if (!port(read, "P2PDC_ECHO_STUN_PORT", 3478, number))
        return "invalid STUN port";
    out.ice.servers.emplace_back(StunServer{*host, number});
    return {};
}
std::optional<std::string> turn(const Environment &read, StandaloneConfig &out) {
    auto host = read("P2PDC_ECHO_TURN_HOST");
    auto policy = read("P2PDC_ECHO_RELAY_POLICY");
    if (policy && *policy != "automatic" && *policy != "required")
        return "invalid relay policy";
    if (policy && *policy == "required")
        out.ice.relay_policy = RelayPolicy::RequireLocalRelay;
    if (!host || host->empty()) {
        if (read("P2PDC_ECHO_TURN_USER") || read("P2PDC_ECHO_TURN_PASSWORD") ||
            read("P2PDC_ECHO_TURN_PORT") || read("P2PDC_ECHO_TURN_TRANSPORT"))
            return "TURN settings need a host";
        return out.ice.relay_policy == RelayPolicy::RequireLocalRelay
                   ? std::optional<std::string>("required relay needs a TURN host")
                   : std::nullopt;
    }
    auto user = read("P2PDC_ECHO_TURN_USER"), password = read("P2PDC_ECHO_TURN_PASSWORD");
    if (!user || !password || user->empty() || password->empty())
        return "TURN username and password are required";
    auto transport = read("P2PDC_ECHO_TURN_TRANSPORT").value_or("udp");
    TurnTransport type = TurnTransport::Udp;
    if (transport == "tcp")
        type = TurnTransport::Tcp;
    else if (transport == "tls")
        type = TurnTransport::Tls;
    else if (transport != "udp")
        return "invalid TURN transport";
    std::uint16_t number = 0;
    if (!port(read, "P2PDC_ECHO_TURN_PORT", type == TurnTransport::Tls ? 5349 : 3478, number))
        return "invalid TURN port";
    out.ice.servers.emplace_back(TurnServer{*host, number, *user, *password, type});
    return {};
}
} // namespace

bool parse_hex(std::string_view text, std::span<std::byte> destination) {
    if (text.size() % 2 != 0 || text.size() / 2 != destination.size())
        return false;
    for (std::size_t index = 0; index < destination.size(); ++index) {
        const auto high = hex_digit(text[index * 2]), low = hex_digit(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        destination[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}
std::variant<StandaloneConfig, std::string> parse_config(const Environment &read) {
    if (!read)
        return std::string("missing environment reader");
    StandaloneConfig out;
    const auto role = read("P2PDC_ECHO_ROLE");
    if (!role || (*role != "initiator" && *role != "responder"))
        return std::string("role must be initiator or responder");
    out.role = *role == "initiator" ? Role::Initiator : Role::Responder;
    if (!read_hex(read, "P2PDC_ECHO_CAPABILITY", out.capability) ||
        !read_hex(read, "P2PDC_ECHO_PEER_KEY", out.authorized_peer) ||
        !read_hex(read, "P2PDC_ECHO_SEED", out.signing_seed) ||
        !read_hex(read, "P2PDC_ECHO_SECRET", out.recipient_secret))
        return std::string("capability, peer key, seed and secret must be nonzero 32-byte hex");
    if (auto error = discovery(read, out))
        return *error;
    if (auto error = stun(read, out))
        return *error;
    if (auto error = turn(read, out))
        return *error;
    return out;
}
} // namespace p2pdc_echo
