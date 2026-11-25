#include "../src/backends/backend_loop.hpp"
#include "../src/backends/backend_records.hpp"
#include "../src/backends/mqtt_codec.hpp"
#include "../src/backends/nostr_event.hpp"
#include "../src/backends/schnorr.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <fstream>
#include <future>
#include <sstream>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
std::vector<unsigned char> unhex(const std::string &text) {
    std::vector<unsigned char> out(text.size() / 2);
    for (std::size_t index = 0; index < out.size(); ++index)
        out[index] = static_cast<unsigned char>(std::stoi(text.substr(index * 2, 2), nullptr, 16));
    return out;
}
template <std::size_t N> std::array<unsigned char, N> fixed(const std::string &text) {
    std::array<unsigned char, N> out{};
    const auto bytes = unhex(text);
    std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
    return out;
}
Bytes as_bytes(std::initializer_list<int> values) {
    Bytes out;
    for (const int value : values)
        out.push_back(static_cast<std::byte>(value));
    return out;
}
} // namespace

TEST(BackendLoop, RunsTasksInOrderAndTimersWhenDue) {
    BackendLoop loop;
    std::vector<int> order;
    std::promise<void> done;
    loop.post_after(60ms, [&] {
        order.push_back(3);
        done.set_value();
    });
    const auto canceled = loop.post_after(30ms, [&] { order.push_back(99); });
    loop.post([&] { order.push_back(1); });
    loop.post([&] { order.push_back(2); });
    loop.cancel(canceled);
    ASSERT_EQ(done.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(BackendLoop, StopsFromItsOwnThreadAndRefusesLaterTasks) {
    auto loop = std::make_shared<BackendLoop>();
    std::promise<bool> inside;
    loop->post([&] {
        loop->stop();
        inside.set_value(loop->on_loop_thread());
    });
    ASSERT_TRUE(inside.get_future().get());
    EXPECT_FALSE(loop->post([] {}));
    EXPECT_EQ(loop->post_after(1ms, [] {}), 0U);
    EXPECT_THROW(static_cast<void>(loop->post({})), std::invalid_argument);
}

TEST(BackendRecords, Base64IsCanonicalAndBounded) {
    const auto value = as_bytes({1, 2, 3, 250});
    const auto text = encode_base64(value);
    EXPECT_EQ(text, "AQID+g==");
    EXPECT_EQ(decode_base64(text, 4), value);
    EXPECT_FALSE(decode_base64(text, 3));       // too large
    EXPECT_FALSE(decode_base64("AQID+h==", 4)); // noncanonical padding bits
    EXPECT_FALSE(decode_base64("AQID+g", 4));   // no padding
    EXPECT_FALSE(decode_base64("not base64!", 64));
    EXPECT_EQ(decode_base64("", 4), Bytes{});
}

TEST(BackendRecords, CollectorDeduplicatesAndDropsBeyondLimits) {
    RecordCollector by_id({8, 3, 64}, false);
    by_id.add({as_bytes({1}), "a"});
    by_id.add({as_bytes({1}), "a"});          // the same record twice
    by_id.add({as_bytes({1}), "b"});          // another writer, same content
    by_id.add({Bytes(9, std::byte{1}), "c"}); // oversized
    by_id.add({as_bytes({2}), "d"});
    by_id.add({as_bytes({3}), "e"}); // beyond max_values
    auto batch = by_id.take();
    EXPECT_EQ(batch.values.size(), 3U);
    EXPECT_EQ(batch.dropped, 2U);
    RecordCollector by_content({8, 8, 64}, true);
    by_content.add({as_bytes({1}), "a"});
    by_content.add({as_bytes({1}), "b"});
    EXPECT_EQ(by_content.take().values.size(), 1U);
    EXPECT_FALSE(record_key(Bytes(20)));
    EXPECT_FALSE(record_key(Bytes(19, std::byte{1})));
    EXPECT_TRUE(record_key(Bytes(20, std::byte{1})));
}

TEST(BackendRecords, RateGateAdmitsAtMostItsLimitPerSecond) {
    RateGate gate(2);
    EXPECT_TRUE(gate.admit());
    EXPECT_TRUE(gate.admit());
    EXPECT_FALSE(gate.admit());
}

// The official BIP340 vectors: signing reproduces each signature, verification each result.
TEST(Schnorr, MatchesTheBip340TestVectors) {
    std::ifstream file(P2PDC_TEST_SOURCE_DIR "/tests/data/bip340-test-vectors.csv");
    ASSERT_TRUE(file);
    std::string line;
    int checked = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line.starts_with("index"))
            continue;
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, ',');)
            fields.push_back(field);
        ASSERT_GE(fields.size(), 7U) << line;
        const auto message = unhex(fields[4]);
        const auto key = fixed<32>(fields[2]);
        const auto signature = fixed<64>(fields[5]);
        if (!fields[1].empty()) {
            const auto secret = fixed<32>(fields[1]);
            EXPECT_EQ(schnorr_public_key(secret), key) << fields[0];
            EXPECT_EQ(schnorr_sign(message, secret, fixed<32>(fields[3])), signature) << fields[0];
        }
        EXPECT_EQ(schnorr_verify(message, key, signature), fields[6] == "TRUE") << fields[0];
        ++checked;
    }
    EXPECT_EQ(checked, 19);
}

TEST(Schnorr, ThrowawaySignersSignVerifiably) {
    const SchnorrSigner signer;
    const std::array<unsigned char, 32> message{7};
    EXPECT_TRUE(schnorr_verify(message, signer.public_key(), signer.sign(message)));
    EXPECT_NE(SchnorrSigner().public_key(), signer.public_key());
}

TEST(NostrEvent, IsSignedOverItsSerializationAndReadsBack) {
    const SchnorrSigner signer;
    RecordKey key{};
    key.fill(std::byte{5});
    const auto value = as_bytes({1, 2, 3});
    const auto event = nostr_event(signer, key, value, 1000);
    auto fields = nlohmann::json::array();
    for (const auto &field : {nlohmann::json(0), event["pubkey"], nlohmann::json(1000),
                              nlohmann::json(nostr_kind), event["tags"], event["content"]})
        fields.push_back(field);
    const auto serial = fields.dump();
    std::array<unsigned char, 32> id{};
    crypto_hash_sha256(id.data(), reinterpret_cast<const unsigned char *>(serial.data()),
                       serial.size());
    EXPECT_EQ(event["id"], to_hex(std::span<const unsigned char>(id)));
    EXPECT_TRUE(schnorr_verify(id, fixed<32>(event["pubkey"]), fixed<64>(event["sig"])));
    const auto hex = to_hex(key);
    const auto record = nostr_record(event, hex, 64, 1000);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->bytes, value);
    EXPECT_FALSE(nostr_record(event, hex, 64, 1600)); // expired
    EXPECT_FALSE(nostr_record(event, hex, 2, 1000));  // too large
    EXPECT_FALSE(nostr_record(event, std::string(40, '0'), 64, 1000));
    auto wrong = event;
    wrong["kind"] = "30078";
    EXPECT_FALSE(nostr_record(wrong, hex, 64, 1000));
    EXPECT_EQ(nostr_refusal("rate-limited: slow down"), BackendFailure::Transient);
    EXPECT_EQ(nostr_refusal("blocked: no"), BackendFailure::Rejected);
}

TEST(MqttCodec, ReassemblesPacketsSplitAcrossMessages) {
    const auto whole = mqtt_publish("p2pdc/v1/aa/bb", 7, Bytes(300, std::byte{5}), 600);
    MqttDecoder decoder(4096);
    const ByteView view(whole);
    EXPECT_EQ(decoder.push(view.first(2))->size(), 0U);
    const auto packets = decoder.push(view.subspan(2));
    ASSERT_TRUE(packets && packets->size() == 1);
    EXPECT_EQ(packets->front().type, static_cast<std::uint8_t>(MqttType::Publish));
    EXPECT_EQ(packets->front().body.size(), whole.size() - 3);
    const auto message = mqtt_message(packets->front().flags, packets->front().body);
    ASSERT_TRUE(message);
    EXPECT_EQ(message->topic, "p2pdc/v1/aa/bb");
    EXPECT_EQ(message->packet_id, 7);
    EXPECT_TRUE(message->retained);
    EXPECT_EQ(message->payload, Bytes(300, std::byte{5}));
}

TEST(MqttCodec, RefusesMalformedOrOversizedStreams) {
    MqttDecoder small(16);
    EXPECT_FALSE(small.push(mqtt_publish("t", 1, Bytes(64), 1)));
    EXPECT_FALSE(small.push(as_bytes({0x30, 0}))); // stays failed
    MqttDecoder endless(4096);
    EXPECT_FALSE(endless.push(as_bytes({0x30, 0xff, 0xff, 0xff, 0xff, 0x01})));
    EXPECT_FALSE(mqtt_ack(static_cast<std::uint8_t>(MqttType::Suback), as_bytes({0})));
    const auto ack =
        mqtt_ack(static_cast<std::uint8_t>(MqttType::Suback), as_bytes({0, 9, 0, 0x87}));
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->packet_id, 9);
    EXPECT_EQ(ack->reason, 0x87);
    EXPECT_EQ(mqtt_refusal(0x97), BackendFailure::Transient);
    EXPECT_EQ(mqtt_refusal(0x87), BackendFailure::Rejected);
}
