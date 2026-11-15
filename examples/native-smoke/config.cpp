#include "config.hpp"

#include <charconv>
#include <cstdlib>
#include <stdexcept>

namespace native_smoke {
Config parse_config(std::string_view mode, const TurnSettings &settings) {
    if (mode == "direct") {
        return {Mode::Direct, {}, 0, {}, {}};
    }
    if (mode != "turn-udp" && mode != "turn-tcp") {
        throw std::invalid_argument("Mode must be direct, turn-udp, or turn-tcp");
    }
    if (settings.host.empty() || settings.port.empty() || settings.username.empty() ||
        settings.password.empty()) {
        throw std::invalid_argument("TURN requires P2PDC_TURN_HOST, P2PDC_TURN_PORT, "
                                    "P2PDC_TURN_USERNAME, and P2PDC_TURN_PASSWORD");
    }
    unsigned int port = 0;
    const auto *end = settings.port.data() + settings.port.size();
    const auto result = std::from_chars(settings.port.data(), end, port);
    if (result.ec != std::errc{} || result.ptr != end || port == 0 || port > 65535) {
        throw std::invalid_argument("P2PDC_TURN_PORT must be an integer from 1 through 65535");
    }
    return {mode == "turn-udp" ? Mode::TurnUdp : Mode::TurnTcp, settings.host,
            static_cast<std::uint16_t>(port), settings.username, settings.password};
}

TurnSettings environment_settings() {
    const auto value = [](const char *name) -> std::string {
        const char *text = std::getenv(name);
        return text == nullptr ? std::string{} : std::string{text};
    };
    return {value("P2PDC_TURN_HOST"), value("P2PDC_TURN_PORT"), value("P2PDC_TURN_USERNAME"),
            value("P2PDC_TURN_PASSWORD")};
}
} // namespace native_smoke
