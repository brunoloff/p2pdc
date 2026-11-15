#include "config.hpp"
#include "smoke.hpp"

#include <rtc/rtc.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("Usage: native_smoke direct|turn-udp|turn-tcp");
        }
        const auto config =
            native_smoke::parse_config(argv[1], native_smoke::environment_settings());
        rtc::InitLogger(rtc::LogLevel::Warning);
        return native_smoke::run_smoke(config) ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "native_smoke: " << error.what() << '\n';
        return 2;
    }
}
