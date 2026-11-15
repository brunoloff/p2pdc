#include <opendht.h>

#ifdef P2PDC_TEST_SECURITY
#include "p2pdc/advertisement.h"
#include "p2pdc/discovery_key.h"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

using namespace std::chrono_literals;

namespace {
class LocalNodes {
  public:
    LocalNodes() {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345678;
        config.peer_discovery = false;
        config.peer_publish = false;
        first_.run("127.0.0.1", "::1", "0", config);
        second_.run("127.0.0.1", "::1", "0", config);
    }
    ~LocalNodes() {
        first_.join();
        second_.join();
    }
    bool connect() {
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        second_.bootstrap(first_.getBound(), [done](bool ok) { done->set_value(ok); });
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
    dht::DhtRunner &first() { return first_; }
    dht::DhtRunner &second() { return second_; }

  private:
    dht::DhtRunner first_;
    dht::DhtRunner second_;
};

bool publish(dht::DhtRunner &runner, dht::InfoHash key, dht::Blob value) {
    auto done = std::make_shared<std::promise<bool>>();
    auto published = done->get_future();
    runner.put(key, dht::Value(std::move(value)), [done](bool ok) { done->set_value(ok); });
    return published.wait_for(15s) == std::future_status::ready && published.get();
}
} // namespace

TEST(OpenDhtIntegration, LocalNodesExchangeBinaryValuesAndReportMissingValues) {
    LocalNodes nodes;
    ASSERT_TRUE(nodes.connect());
    const auto key = dht::InfoHash::get("p2pdc-local-integration-test");
    const dht::Blob expected{0, 1, 127, 255};
    ASSERT_TRUE(publish(nodes.first(), key, expected));
    auto lookup = nodes.second().get(key);
    ASSERT_EQ(lookup.wait_for(15s), std::future_status::ready);
    auto values = lookup.get();
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front()->data, expected);
    auto missing = nodes.second().get(dht::InfoHash::get("p2pdc-local-missing-test"));
    ASSERT_EQ(missing.wait_for(15s), std::future_status::ready);
    EXPECT_TRUE(missing.get().empty());
}

TEST(OpenDhtIntegration, LocalListenerReceivesPublishedValue) {
    LocalNodes nodes;
    ASSERT_TRUE(nodes.connect());
    const auto key = dht::InfoHash::get("p2pdc-local-listener-test");
    const dht::Blob expected{0, 255, 42};
    auto received = std::make_shared<std::promise<dht::Blob>>();
    auto delivered = std::make_shared<std::atomic_bool>(false);
    auto future = received->get_future();
    dht::ValueCallback callback = [received, delivered](const auto &values, bool expired) {
        if (!expired && !values.empty() && !delivered->exchange(true)) {
            received->set_value(values.front()->data);
        }
        return true;
    };
    auto token = nodes.second().listen(key, callback).share();
    ASSERT_EQ(token.wait_for(10s), std::future_status::ready);
    nodes.first().put(key, dht::Value(expected));
    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    EXPECT_EQ(future.get(), expected);
    nodes.second().cancelListen(key, token);
}

#ifdef P2PDC_TEST_SECURITY
TEST(OpenDhtIntegration, EncryptedAdvertisementSurvivesLocalDhtExchange) {
    using namespace p2pdc;
    LocalNodes nodes;
    ASSERT_TRUE(nodes.connect());
    auto identity_result = Identity::generate();
    auto capability_result = generate_discovery_capability();
    ASSERT_TRUE(std::holds_alternative<Identity>(identity_result));
    ASSERT_TRUE(std::holds_alternative<DiscoveryCapability>(capability_result));
    const auto &identity = std::get<Identity>(identity_result);
    const auto &capability = std::get<DiscoveryCapability>(capability_result);
    const auto lookup_key = std::get<DiscoveryKey>(derive_discovery_key(capability));
    const dht::InfoHash key(reinterpret_cast<const std::uint8_t *>(lookup_key.data()),
                            lookup_key.size());
    AdvertisementFields fields{};
    fields.issued_at = 100;
    fields.expires_at = 200;
    fields.recipient_public_key[0] = std::byte{1};
    fields.instance_id[0] = std::byte{2};
    const AdvertisementPolicy policy{150, 200, 0};
    const auto encoded = encode_advertisement(identity, fields, capability, policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    const auto &wire = std::get<Bytes>(encoded);
    const dht::Blob value(reinterpret_cast<const std::uint8_t *>(wire.data()),
                          reinterpret_cast<const std::uint8_t *>(wire.data()) + wire.size());
    ASSERT_TRUE(publish(nodes.first(), key, value));
    auto lookup = nodes.second().get(key);
    ASSERT_EQ(lookup.wait_for(15s), std::future_status::ready);
    const auto records = lookup.get();
    ASSERT_EQ(records.size(), 1U);
    const ByteView received(reinterpret_cast<const std::byte *>(records.front()->data.data()),
                            records.front()->data.size());
    const auto decoded = decode_advertisement(received, capability, policy);
    ASSERT_TRUE(std::holds_alternative<Advertisement>(decoded));
    EXPECT_EQ(std::get<Advertisement>(decoded).fields, fields);
    auto wrong_capability = capability;
    wrong_capability[0] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(decode_advertisement(received, wrong_capability, policy)),
              CryptoError::AuthenticationFailed);
}
#endif
