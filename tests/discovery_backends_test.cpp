#include "../src/discovery/discovery_backends_json.hpp"
#include "p2pdc/discovery_backends.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace p2pdc;

TEST(DiscoveryBackendsTest, TheOneLineFormReadsBackWhatItWrites) {
    const std::vector<DiscoveryBackend> backends{
        OpenDhtProxies{{"https://dhtproxy.jami.net", "https://proxy.example:8443/base"}},
        NostrRelay{"wss://relay.example"}, MqttBroker{"wss://broker.example:8084/mqtt"},
        PeerJsServer{"wss://peers.example/peerjs", "secret"},
        PeerJsServer{"ws://127.0.0.1:9000/peerjs"}};
    for (const auto &backend : backends) {
        const auto text = to_string(backend);
        const auto parsed = parse_discovery_backend(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(*parsed, backend) << text;
    }
    EXPECT_EQ(to_string(backends[1]), "nostr wss://relay.example");
    EXPECT_EQ(parse_discovery_backend("  peerjs   wss://peers.example/peerjs "),
              DiscoveryBackend(PeerJsServer{"wss://peers.example/peerjs", "peerjs"}));
}

TEST(DiscoveryBackendsTest, RefusesWhatBrowsersCouldNotUse) {
    for (const char *text :
         {"", "   ", "nostr", "nostr wss://a.example wss://b.example", "ftp wss://a.example",
          "nostr https://relay.example", "nostr ws://relay.example",
          "mqtt wss://user:pass@broker.example", "opendht wss://proxy.example",
          "opendht http://proxy.example", "opendht", "nostr wss://relay.example#x",
          "peerjs wss://peers.example a b", "mqtt wss://"}) {
        EXPECT_FALSE(parse_discovery_backend(text).has_value()) << text;
    }
    EXPECT_TRUE(parse_discovery_backend("opendht http://localhost:8080").has_value());
    EXPECT_TRUE(parse_discovery_backend("nostr ws://[::1]:7000").has_value());
    EXPECT_FALSE(valid_discovery_backend(
        OpenDhtProxies{std::vector<std::string>(9, "https://proxy.example")}));
    EXPECT_FALSE(valid_discovery_backend(PeerJsServer{"wss://peers.example", "with \"quote"}));
}

TEST(DiscoveryBackendsTest, TheBridgeGetsAJsonListOfOneToEightValidBackends) {
    EXPECT_EQ(
        detail::discovery_backends_json({OpenDhtProxies{{"https://a.example", "https://b.example"}},
                                       NostrRelay{"wss://r.example"},
                                       MqttBroker{"wss://m.example/mqtt"},
                                       PeerJsServer{"wss://p.example/peerjs", "k"}}),
        R"([{"kind":"opendht","urls":["https://a.example","https://b.example"]},)"
        R"({"kind":"nostr","url":"wss://r.example"},{"kind":"mqtt","url":"wss://m.example/mqtt"},)"
        R"({"kind":"peerjs","url":"wss://p.example/peerjs","key":"k"}])");
    EXPECT_THROW(static_cast<void>(detail::discovery_backends_json({})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(detail::discovery_backends_json(
                     std::vector<DiscoveryBackend>(9, NostrRelay{"wss://r.example"}))),
                 std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(detail::discovery_backends_json({NostrRelay{"https://r.example"}})),
        std::invalid_argument);
}
