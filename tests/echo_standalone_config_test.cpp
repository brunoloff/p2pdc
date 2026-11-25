#include "../examples/echo/standalone_config.hpp"

#include <gtest/gtest.h>

#include <map>

using namespace p2pdc;
using namespace p2pdc_echo;
namespace {
using Values = std::map<std::string, std::string>;
Values valid() {
    return {{"P2PDC_ECHO_ROLE", "initiator"},
            {"P2PDC_ECHO_CAPABILITY", std::string(64, '1')},
            {"P2PDC_ECHO_PEER_KEY", std::string(64, '2')},
            {"P2PDC_ECHO_SEED", std::string(64, '3')},
            {"P2PDC_ECHO_SECRET", std::string(64, '4')},
            {"P2PDC_ECHO_BOOTSTRAP_HOST", "example.test"},
            {"P2PDC_ECHO_PROXY", "https://example.test"}};
}
Environment reader(const Values &values) {
    return [&values](std::string_view name) -> std::optional<std::string> {
        auto found = values.find(std::string(name));
        if (found == values.end())
            return {};
        return found->second;
    };
}
} // namespace

TEST(EchoStandaloneConfigTest, ParsesSharedDiscoveryAndTurnSettings) {
    auto values = valid();
    values["P2PDC_ECHO_NETWORK"] = "42";
    values["P2PDC_ECHO_STUN_HOST"] = "stun.test";
    values["P2PDC_ECHO_STUN_PORT"] = "3479";
    values["P2PDC_ECHO_TURN_HOST"] = "relay.test";
    values["P2PDC_ECHO_TURN_USER"] = "user";
    values["P2PDC_ECHO_TURN_PASSWORD"] = "secret";
    values["P2PDC_ECHO_TURN_TRANSPORT"] = "tls";
    values["P2PDC_ECHO_RELAY_POLICY"] = "required";
    auto parsed = parse_config(reader(values));
    ASSERT_TRUE(std::holds_alternative<StandaloneConfig>(parsed));
    auto &config = std::get<StandaloneConfig>(parsed);
    EXPECT_EQ(config.discovery.network_id, 42U);
    EXPECT_EQ(config.discovery.bootstrap_servers.front().port, 4222);
    EXPECT_EQ(config.discovery.discovery_backends.front(),
              p2pdc::DiscoveryBackend(p2pdc::OpenDhtProxies{{"https://example.test"}}));
    EXPECT_EQ(config.ice.relay_policy, RelayPolicy::RequireLocalRelay);
    EXPECT_EQ(std::get<StunServer>(config.ice.servers.front()).port, 3479);
    const auto &turn = std::get<TurnServer>(config.ice.servers.back());
    EXPECT_EQ(turn.port, 5349);
    EXPECT_EQ(turn.transport, TurnTransport::Tls);
}

TEST(EchoStandaloneConfigTest, RejectsMalformedSecretsAndMissingFields) {
    std::array<std::byte, 2> bytes{};
    EXPECT_TRUE(parse_hex("00fF", bytes));
    EXPECT_EQ(bytes[1], std::byte{255});
    EXPECT_FALSE(parse_hex("0x00", bytes));
    EXPECT_FALSE(parse_hex("000", bytes));
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config({})));
    auto values = valid();
    values["P2PDC_ECHO_SEED"] = std::string(64, '0');
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
    values = valid();
    values.erase("P2PDC_ECHO_PEER_KEY");
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
}

TEST(EchoStandaloneConfigTest, RejectsInvalidNetworkAndRelaySettings) {
    auto values = valid();
    values["P2PDC_ECHO_BOOTSTRAP_PORT"] = "65536";
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
    values = valid();
    values["P2PDC_ECHO_NETWORK"] = "4294967296";
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
    values = valid();
    values["P2PDC_ECHO_RELAY_POLICY"] = "required";
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
    values = valid();
    values["P2PDC_ECHO_STUN_PORT"] = "3478";
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
    values = valid();
    values["P2PDC_ECHO_TURN_USER"] = "orphan";
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
    values = valid();
    values.erase("P2PDC_ECHO_BOOTSTRAP_HOST");
    values.erase("P2PDC_ECHO_PROXY");
    EXPECT_TRUE(std::holds_alternative<std::string>(parse_config(reader(values))));
}
