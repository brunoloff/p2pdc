#pragma once

#include "backend_records.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace p2pdc::detail {

// The few MQTT 5 packets a record backend needs (OASIS MQTT 5.0, section 3), a port of
// mqtt_codec.mjs.
enum class MqttType : std::uint8_t {
    Connect = 1,
    Connack = 2,
    Publish = 3,
    Puback = 4,
    Subscribe = 8,
    Suback = 9,
    Unsubscribe = 10,
    Unsuback = 11,
    Pingreq = 12,
    Pingresp = 13,
    Disconnect = 14
};

[[nodiscard]] Bytes mqtt_connect(const std::string &client_id, std::uint16_t keep_alive_seconds);
// QoS 1, so that the broker acknowledges keeping it; retained, expiring after seconds.
[[nodiscard]] Bytes mqtt_publish(const std::string &topic, std::uint16_t packet_id,
                                 ByteView payload, std::uint32_t expiry_seconds);
// Retain handling 0 replays stored messages on every subscribe, 2 never does; QoS 0.
[[nodiscard]] Bytes mqtt_subscribe(std::uint16_t packet_id, const std::string &filter,
                                   bool replay_retained);
[[nodiscard]] Bytes mqtt_unsubscribe(std::uint16_t packet_id, const std::string &filter);
[[nodiscard]] Bytes mqtt_ping();
[[nodiscard]] Bytes mqtt_puback(std::uint16_t packet_id);

struct MqttPacket {
    std::uint8_t type;
    std::uint8_t flags;
    Bytes body;
};

// Splits a byte stream into packets. Nullopt, for good, once the stream is malformed or a
// packet exceeds max_packet_bytes.
class MqttDecoder {
  public:
    explicit MqttDecoder(std::size_t max_packet_bytes) : max_(max_packet_bytes) {}
    [[nodiscard]] std::optional<std::vector<MqttPacket>> push(ByteView chunk);

  private:
    std::size_t max_;
    Bytes buffer_;
    bool failed_ = false;
};

// The reason code of a CONNACK, PUBACK or SUBACK, and its packet identifier (zero for CONNACK).
struct MqttAck {
    std::uint16_t packet_id = 0;
    std::uint8_t reason = 0;
};
[[nodiscard]] std::optional<MqttAck> mqtt_ack(std::uint8_t type, ByteView body);

struct MqttMessage {
    std::string topic;
    std::optional<std::uint16_t> packet_id;
    bool retained = false;
    Bytes payload;
};
[[nodiscard]] std::optional<MqttMessage> mqtt_message(std::uint8_t flags, ByteView body);

// What an error reason code means for trying again (MQTT 5, 2.4): server busy, quota or rate
// exceeded may pass.
[[nodiscard]] BackendFailure mqtt_refusal(std::uint8_t reason);

} // namespace p2pdc::detail
