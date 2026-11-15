#include "../src/browser/browser_ice_config.hpp"
#include "../src/transport/native_backend_features.hpp"
#include "../src/transport/native_ice_config.hpp"

#include <gtest/gtest.h>
#include <rtc/global.hpp>

#include <cstdlib>
#include <string_view>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(NativeIceConfig, RejectsTurnTlsWhenBackendCannotGuaranteeEncryption) {
    IceConfig config;
    config.servers = {TurnServer{"relay.example", 5349, "user", "secret", TurnTransport::Tls}};
    EXPECT_THROW((void)make_native_ice_config(config, 0), std::invalid_argument);
    auto native = make_native_ice_config(config, native_turn_tls);
    ASSERT_EQ(native.rtc.iceServers.size(), 1U);
    EXPECT_EQ(native.rtc.iceServers[0].hostname, "relay.example");
    EXPECT_EQ(native.rtc.iceServers[0].relayType, rtc::IceServer::RelayType::TurnTls);
    EXPECT_NO_THROW((void)browser_ice_servers(config));
}

TEST(NativeIceConfig, UsesTheLoadedBackendTlsCapability) {
    IceConfig config;
    config.servers = {TurnServer{"relay.example", 5349, "user", "secret", TurnTransport::Tls}};
    const bool supported = (rtc::P2pdcBackendFeatures() & native_turn_tls) != 0;
    if (const char *expected = std::getenv("P2PDC_TEST_EXPECT_TLS")) {
        ASSERT_TRUE(std::string_view(expected) == "yes" || std::string_view(expected) == "no");
        EXPECT_EQ(supported, std::string_view(expected) == "yes");
    }
    if (supported)
        EXPECT_NO_THROW((void)make_native_ice_config(config));
    else
        EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
}

TEST(NativeIceConfig, MapsStunAndSupportedTurnTransportsWithoutLosingCredentials) {
    IceConfig config;
    config.relay_policy = RelayPolicy::RequireLocalRelay;
    config.servers = {StunServer{"stun.example", 3478},
                      TurnServer{"udp.example", 3478, "user", "secret", TurnTransport::Udp},
                      TurnServer{"tcp.example", 3478, "user", "secret", TurnTransport::Tcp}};
    const auto native = make_native_ice_config(config);
    EXPECT_EQ(native.policy, NativeRelayPolicy::RequireLocalRelay);
    ASSERT_EQ(native.rtc.iceServers.size(), 3U);
    EXPECT_EQ(native.rtc.iceServers[0].type, rtc::IceServer::Type::Stun);
    EXPECT_EQ(native.rtc.iceServers[1].relayType, rtc::IceServer::RelayType::TurnUdp);
    EXPECT_EQ(native.rtc.iceServers[2].relayType, rtc::IceServer::RelayType::TurnTcp);
    EXPECT_EQ(native.rtc.iceServers[2].username, "user");
    EXPECT_EQ(native.rtc.iceServers[2].password, "secret");
}

TEST(NativeIceConfig, AllowsAutomaticWithoutServersAndAnonymousTurn) {
    EXPECT_EQ(make_native_ice_config({}).policy, NativeRelayPolicy::Automatic);
    IceConfig config;
    config.servers = {TurnServer{"relay.example", 3478, "", "", TurnTransport::Udp}};
    EXPECT_EQ(make_native_ice_config(config).rtc.iceServers.size(), 1U);
}

TEST(NativeIceConfig, RejectsInvalidServersAndMissingRelay) {
    IceConfig config;
    config.servers = {StunServer{"", 3478}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {StunServer{"bad host", 3478}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {StunServer{"stun.example", 0}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {TurnServer{"relay.example", 3478, "user", "", TurnTransport::Udp}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {
        TurnServer{"relay.example", 3478, std::string("u\0x", 3), "secret", TurnTransport::Udp}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {
        TurnServer{"relay.example", 3478, "user", std::string("p\0x", 3), TurnTransport::Udp}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {StunServer{"stun://example", 3478}};
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.servers = {StunServer{"stun.example", 3478}};
    config.relay_policy = RelayPolicy::RequireLocalRelay;
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
}

TEST(NativeIceConfig, RejectsUnknownEnumsAndInsufficientBackendFeatures) {
    IceConfig config;
    // Deliberate malformed enum values exercise validation of external inputs.
    config.relay_policy =
        static_cast<RelayPolicy>(100); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    config.relay_policy = RelayPolicy::Automatic;
    config.servers = {TurnServer{
        "relay.example", 3478, "user", "secret",
        static_cast<TurnTransport>(100)}}; // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
    EXPECT_FALSE(supports_native_backend(native_send_admission, false, true));
    EXPECT_FALSE(supports_native_backend(native_send_admission | native_force_relay, false, true));
    EXPECT_TRUE(supports_native_backend(
        native_send_admission | native_force_relay | native_safe_ice_detach, false, true));
    EXPECT_TRUE(
        supports_native_backend(native_send_admission | native_safe_ice_detach, false, false));
}

TEST(NativeIceConfig, NativeAndBrowserApplyTheSameConfigurationBounds) {
    IceConfig valid;
    valid.servers = std::vector<IceServer>(16, StunServer{std::string(253, 'a'), 3478});
    EXPECT_NO_THROW((void)make_native_ice_config(valid));
    EXPECT_NO_THROW((void)browser_ice_servers(valid));
    valid.servers = {TurnServer{"::1", 3478, std::string(1024, 'u'), std::string(1024, 'p'),
                                TurnTransport::Tcp}};
    EXPECT_NO_THROW((void)make_native_ice_config(valid));
    EXPECT_NO_THROW((void)browser_ice_servers(valid));
    std::vector<IceConfig> invalid{
        {std::vector<IceServer>(17, StunServer{"host", 3478}), RelayPolicy::Automatic},
        {{StunServer{std::string(254, 'a'), 3478}}, RelayPolicy::Automatic},
        {{StunServer{"host:5", 3478}}, RelayPolicy::Automatic},
        {{StunServer{":::1", 3478}}, RelayPolicy::Automatic},
        {{TurnServer{"host", 3478, std::string(1025, 'u'), "p", TurnTransport::Udp}},
         RelayPolicy::Automatic},
        {{TurnServer{"host", 3478, "u", std::string(1025, 'p'), TurnTransport::Udp}},
         RelayPolicy::Automatic}};
    for (const auto &config : invalid) {
        EXPECT_THROW((void)make_native_ice_config(config), std::invalid_argument);
        EXPECT_THROW((void)browser_ice_servers(config), std::invalid_argument);
    }
}
