#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace native_smoke {
enum class Mode { Direct, TurnUdp, TurnTcp };
struct TurnSettings {
    std::string host;
    std::string port;
    std::string username;
    std::string password;
};
struct Config {
    Mode mode;
    std::string host;
    std::uint16_t port;
    std::string username;
    std::string password;
};
Config parse_config(std::string_view mode, const TurnSettings &settings);
TurnSettings environment_settings();
} // namespace native_smoke
