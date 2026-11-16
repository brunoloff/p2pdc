#include "native_discovery_watch_fixture.hpp"
#include "p2pdc/node.h"
#include "p2pdc/recipient_crypto.h"

#include <algorithm>

using namespace p2pdc;

namespace {
TEST(NodeIdentityTest, ImportsStableKeysAndRejectsInvalidInputs) {
    const auto executor = [](std::function<void()>) {};
    Bytes seed(32, std::byte{1}), secret(32, std::byte{3});
    auto first = Node::create_with_identity({}, executor, seed, secret);
    auto second = Node::create_with_identity({}, executor, seed, secret);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Node>>(first));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Node>>(second));
    auto a = std::move(std::get<std::unique_ptr<Node>>(first));
    auto b = std::move(std::get<std::unique_ptr<Node>>(second));
    ASSERT_TRUE(a->identity());
    EXPECT_EQ(a->identity(), b->identity());
    auto signing = Identity::import_seed(seed);
    auto recipient = RecipientKeyPair::import_secret_key(secret);
    ASSERT_TRUE(std::holds_alternative<Identity>(signing));
    ASSERT_TRUE(std::holds_alternative<RecipientKeyPair>(recipient));
    EXPECT_EQ(a->identity()->signing_key, std::get<Identity>(signing).public_key());
    EXPECT_EQ(a->identity()->recipient_key, std::get<RecipientKeyPair>(recipient).public_key());
    std::fill(seed.begin(), seed.end(), std::byte{});
    std::fill(secret.begin(), secret.end(), std::byte{});
    EXPECT_EQ(a->identity(), b->identity());
    EXPECT_EQ(std::get<CryptoError>(Node::create_with_identity({}, executor, Bytes(31), secret)),
              CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(Node::create_with_identity({}, executor, seed, Bytes(31))),
              CryptoError::InvalidLength);
    EXPECT_THROW(static_cast<void>(Node::create_with_identity({}, {}, seed, secret)),
                 std::invalid_argument);
    auto ephemeral = Node::create({}, executor);
    EXPECT_FALSE(ephemeral->identity());
}

class NativeNodeIdentityTest : public NativeDiscoveryWatchTest {
  protected:
    std::vector<std::function<void()>> timers;
    std::size_t next_timer = 0;
    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            timers.push_back(std::move(callback));
            return [] {};
        };
    }
    bool advance(const std::function<bool()> &done) {
        return loop.until([&] {
            if (done())
                return true;
            if (next_timer < timers.size())
                timers[next_timer++]();
            return false;
        });
    }
};

TEST_F(NativeNodeIdentityTest, AdvertisesImportedPublicKeysAndClearsOnShutdown) {
    NativeDiscoveryConfig config;
    config.network_id = 0x12345684;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    Bytes seed(32, std::byte{1}), secret(32, std::byte{3});
    auto result = Node::create_with_identity(config, loop.executor(), seed, secret);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Node>>(result));
    auto node = std::move(std::get<std::unique_ptr<Node>>(result));
    ASSERT_TRUE(node->identity());
    const auto expected = *node->identity();
    std::fill(seed.begin(), seed.end(), std::byte{});
    std::fill(secret.begin(), secret.end(), std::byte{});
    bool started = false, published = false, stopped = false;
    ASSERT_TRUE(
        node->start([&](auto value) { started = value.status == NodeStartStatus::Started; }));
    ASSERT_TRUE(loop.until([&] { return started; }));
    std::optional<Advertisement> observed;
    DiscoveryCapability capability_value{};
    std::copy(capability->begin(), capability->end(), capability_value.begin());
    auto watched = node->watch(capability_value, scheduler(), {},
                               {[&](Advertisement peer, PeerEvent event) {
                                    if (event == PeerEvent::Seen)
                                        observed = std::move(peer);
                                },
                                [](bool) {}, [](WatchError) { ADD_FAILURE() << "watch failed"; }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(watched));
    auto watch = std::move(std::get<std::unique_ptr<Subscription>>(watched));
    auto announced = node->announce(
        capability_value, scheduler(), {},
        {[&] { published = true; }, [](AnnouncementError) { ADD_FAILURE() << "announce failed"; }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(announced));
    auto lease = std::move(std::get<std::unique_ptr<Lease>>(announced));
    ASSERT_TRUE(advance([&] { return published && observed.has_value(); }));
    EXPECT_EQ(observed->signing_public_key, expected.signing_key);
    EXPECT_EQ(observed->fields.recipient_public_key, expected.recipient_key);
    ASSERT_TRUE(
        node->shutdown([&](auto value) { stopped = value.status == NodeStopStatus::Stopped; }));
    ASSERT_TRUE(loop.until([&] { return stopped; }));
    EXPECT_FALSE(node->identity());
}
TEST_F(NativeNodeIdentityTest, FreshIdentityLeaseDoesNotAdvertiseTheNodeKeys) {
    NativeDiscoveryConfig config;
    config.network_id = 0x12345684;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", first->getBoundPort(AF_INET)}};
    Bytes seed(32, std::byte{1}), secret(32, std::byte{3});
    auto result = Node::create_with_identity(config, loop.executor(), seed, secret);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Node>>(result));
    auto node = std::move(std::get<std::unique_ptr<Node>>(result));
    const auto node_identity = *node->identity();
    bool started = false, published = false, stopped = false;
    ASSERT_TRUE(
        node->start([&](auto value) { started = value.status == NodeStartStatus::Started; }));
    ASSERT_TRUE(loop.until([&] { return started; }));
    std::optional<Advertisement> observed;
    DiscoveryCapability capability_value{};
    std::copy(capability->begin(), capability->end(), capability_value.begin());
    auto watched = node->watch(capability_value, scheduler(), {},
                               {[&](Advertisement peer, PeerEvent event) {
                                    if (event == PeerEvent::Seen)
                                        observed = std::move(peer);
                                },
                                [](bool) {}, [](WatchError) { ADD_FAILURE() << "watch failed"; }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(watched));
    auto watch = std::move(std::get<std::unique_ptr<Subscription>>(watched));
    AnnounceOptions options;
    options.fresh_identity = true;
    auto announced = node->announce(
        capability_value, scheduler(), options,
        {[&] { published = true; }, [](AnnouncementError) { ADD_FAILURE() << "announce failed"; }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Lease>>(announced));
    auto lease = std::move(std::get<std::unique_ptr<Lease>>(announced));
    ASSERT_TRUE(advance([&] { return published && observed.has_value(); }));
    EXPECT_NE(observed->signing_public_key, node_identity.signing_key);
    EXPECT_NE(observed->fields.recipient_public_key, node_identity.recipient_key);
    EXPECT_EQ(node->identity()->signing_key, node_identity.signing_key);
    ASSERT_TRUE(
        node->shutdown([&](auto value) { stopped = value.status == NodeStopStatus::Stopped; }));
    ASSERT_TRUE(loop.until([&] { return stopped; }));
}
} // namespace
