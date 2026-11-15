#include <opendht.h>
#include <opendht/securedht.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {
constexpr dht::Value::Id signed_id = 201;
constexpr dht::Value::Id unsigned_id = 202;

template <class T> T await(std::future<T> &future) {
    if (future.wait_for(15s) != std::future_status::ready) {
        throw std::runtime_error("timed out waiting for local OpenDHT callback");
    }
    return future.get();
}

class OpenDhtReplacementTest : public ::testing::Test {
  protected:
    std::shared_ptr<dht::DhtRunner> writer = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> reader = std::make_shared<dht::DhtRunner>();
    dht::crypto::Identity owner = dht::crypto::generateIdentity("replacement-owner", {}, 2048);
    dht::crypto::Identity attacker =
        dht::crypto::generateIdentity("replacement-attacker", {}, 2048);
    const dht::InfoHash key = dht::InfoHash::get("p2pdc-replacement-test");

    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345685;
        config.dht_config.id = owner;
        config.peer_discovery = false;
        config.peer_publish = false;
        writer->run("127.0.0.1", "::1", "0", config);
        config.dht_config.id = {};
        reader->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        reader->bootstrap(writer->getBound(), [done](bool success) { done->set_value(success); });
        ASSERT_TRUE(await(future));
        reader->registerCertificate(owner.second);
        reader->registerCertificate(attacker.second);
    }
    void TearDown() override {
        writer->join();
        reader->join();
    }
    bool put_signed(dht::Value value) {
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        writer->putSigned(key, std::move(value),
                          [done](bool success) { done->set_value(success); });
        return await(future);
    }
    std::vector<std::shared_ptr<dht::Value>> get() {
        auto future = reader->get(key);
        return await(future);
    }
    static std::string describe(const std::vector<std::shared_ptr<dht::Value>> &values) {
        std::ostringstream output;
        for (const auto &item : values) {
            output << "id=" << item->id << " seq=" << item->seq << " signed=" << item->isSigned()
                   << " bytes=" << item->data.size();
            if (!item->data.empty())
                output << " first=" << static_cast<unsigned>(item->data[0]);
            output << ';';
        }
        return output.str();
    }
    dht::Value value(dht::Value::Id id, std::uint16_t sequence, std::uint8_t byte,
                     const dht::crypto::Identity &signer) {
        dht::Value result(dht::Blob{byte});
        result.id = id;
        result.seq = sequence;
        if (signer.first)
            result.sign(*signer.first);
        return result;
    }
};
} // namespace

TEST_F(OpenDhtReplacementTest, SignedValueCanBeUpdatedByOwner) {
    const auto signed_value = value(signed_id, 1, 10, owner);
    ASSERT_TRUE(signed_value.checkSignature());
    ASSERT_TRUE(put_signed(value(signed_id, 1, 10, {})));
    ASSERT_TRUE(put_signed(value(signed_id, 2, 20, {})));
    auto values = get();
    auto writer_future = writer->get(key);
    auto writer_values = await(writer_future);
    ASSERT_EQ(values.size(), 1U) << describe(values) << " local=" << reader->getStorageLog(key)
                                 << " writer=" << describe(writer_values)
                                 << " writer_local=" << writer->getStorageLog(key);
    ASSERT_EQ(values.front()->data, (dht::Blob{20}));
    EXPECT_EQ(values.front()->seq, 2U);
}

TEST_F(OpenDhtReplacementTest, EditPolicyChecksOwnerSequenceAndUnsignedValues) {
    dht::SecureDht secure(nullptr, {});
    const auto type = secure.secureType(dht::ValueType::USER_DATA);
    const auto old = std::make_shared<dht::Value>(value(signed_id, 2, 10, owner));
    auto update = std::make_shared<dht::Value>(value(signed_id, 3, 20, owner));
    ASSERT_TRUE(old->checkSignature());
    ASSERT_TRUE(update->checkSignature());
    EXPECT_TRUE(type.editPolicy(key, old, update, {}, {}));
    update = std::make_shared<dht::Value>(value(signed_id, 2, 30, owner));
    EXPECT_FALSE(type.editPolicy(key, old, update, {}, {}));
    update = std::make_shared<dht::Value>(value(signed_id, 3, 40, attacker));
    EXPECT_FALSE(type.editPolicy(key, old, update, {}, {}));
    const auto unsigned_old = std::make_shared<dht::Value>(value(unsigned_id, 1, 10, {}));
    update = std::make_shared<dht::Value>(value(unsigned_id, 2, 20, {}));
    EXPECT_FALSE(type.editPolicy(key, unsigned_old, update, {}, {}));
}
