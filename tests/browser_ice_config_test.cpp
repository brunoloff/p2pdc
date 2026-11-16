#include "../src/browser/browser_ice_config.hpp"
#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(BrowserIceConfig, MapsServersAndRelayPolicy) {
    IceConfig config{
        {StunServer{"127.0.0.1", 3478}, TurnServer{"::1", 5349, "u", "p", TurnTransport::Tls}},
        RelayPolicy::RequireLocalRelay};
    auto servers = browser_ice_servers(config);
    ASSERT_EQ(servers.size(), 2U);
    EXPECT_EQ(servers[0].url, "stun:127.0.0.1:3478");
    EXPECT_EQ(servers[1].url, "turns:[::1]:5349?transport=tcp");
    EXPECT_EQ(servers[1].username, "u");
    EXPECT_EQ(servers[1].password, "p");
}
TEST(BrowserIceConfig, RejectsInvalidConfigurations) {
    EXPECT_THROW((void)browser_ice_servers({{}, RelayPolicy::RequireLocalRelay}),
                 std::invalid_argument);
    EXPECT_THROW((void)browser_ice_servers({{StunServer{"host:5", 3478}}, RelayPolicy::Automatic}),
                 std::invalid_argument);
    EXPECT_THROW((void)browser_ice_servers({{TurnServer{"host", 3478, "u", "", TurnTransport::Udp}},
                                            RelayPolicy::Automatic}),
                 std::invalid_argument);
    EXPECT_THROW((void)browser_ice_servers({std::vector<IceServer>(17, StunServer{"host", 3478}),
                                            RelayPolicy::Automatic}),
                 std::invalid_argument);
    for (const char *host : {"turn.example;transport=tcp", "host%3Fx", "a,b", "host\\x", "h\"x",
                             "bad host", "user@host"})
        EXPECT_THROW((void)browser_ice_servers({{StunServer{host, 3478}}, RelayPolicy::Automatic}),
                     std::invalid_argument)
            << host;
    EXPECT_NO_THROW((void)browser_ice_servers(
        {{StunServer{"turn-1.example_net.org", 3478}, StunServer{"2001:db8::1", 3478}},
         RelayPolicy::Automatic}));
}
