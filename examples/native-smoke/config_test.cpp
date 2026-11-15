#include "config.hpp"
#include "smoke.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

namespace native_smoke {
TEST(Config, DirectRequiresNoTurnSettings) {
    EXPECT_EQ(parse_config("direct", {}).mode, Mode::Direct);
}
TEST(Config, ParsesBothTurnTransports) {
    const TurnSettings settings{"turn.example.test", "3478", "user", "test-password"};
    const auto udp = parse_config("turn-udp", settings);
    EXPECT_EQ(udp.mode, Mode::TurnUdp);
    EXPECT_EQ(udp.host, settings.host);
    EXPECT_EQ(udp.port, 3478);
    EXPECT_EQ(udp.username, settings.username);
    EXPECT_EQ(udp.password, settings.password);
    EXPECT_EQ(parse_config("turn-tcp", settings).mode, Mode::TurnTcp);
}
TEST(Config, RejectsUnknownModeAndMissingCredentials) {
    EXPECT_THROW(parse_config("invalid", {}), std::invalid_argument);
    EXPECT_THROW(parse_config("turn-udp", {}), std::invalid_argument);
    EXPECT_THROW(parse_config("turn-tcp", {"host", "3478", "user", ""}), std::invalid_argument);
}
TEST(Config, RejectsMalformedAndOutOfRangePorts) {
    for (const char *port : {"0", "65536", "-1", "3478suffix", "99999999999999999999"}) {
        EXPECT_THROW(parse_config("turn-udp", {"host", port, "user", "password"}),
                     std::invalid_argument);
    }
    EXPECT_EQ(parse_config("turn-udp", {"host", "65535", "user", "password"}).port, 65535);
}
TEST(Config, EnvironmentSettingsDoesNotAffectDirectMode) {
    const auto settings = environment_settings();
    EXPECT_NO_THROW(parse_config("direct", settings));
}
TEST(Route, RequiresKnownCandidatesForDirectButDetectsEitherRelay) {
    using Type = rtc::Candidate::Type;
    EXPECT_EQ(classify_pair(false, Type::Host, Type::Host), Route::Unknown);
    EXPECT_EQ(classify_pair(true, Type::Unknown, Type::Host), Route::Unknown);
    EXPECT_EQ(classify_pair(true, Type::PeerReflexive, Type::Host), Route::Unknown);
    EXPECT_EQ(classify_pair(true, Type::Host, Type::PeerReflexive), Route::Unknown);
    EXPECT_EQ(classify_pair(true, Type::Relayed, Type::PeerReflexive), Route::TurnRelayed);
    EXPECT_EQ(classify_pair(true, Type::Host, Type::ServerReflexive), Route::Direct);
    EXPECT_EQ(classify_pair(true, Type::Relayed, Type::Unknown), Route::TurnRelayed);
    EXPECT_EQ(classify_pair(true, Type::Host, Type::Relayed), Route::TurnRelayed);
}
} // namespace native_smoke
