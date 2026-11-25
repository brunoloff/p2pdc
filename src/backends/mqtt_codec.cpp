#include "mqtt_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace p2pdc::detail {
namespace {
constexpr std::uint8_t message_expiry = 0x02;

class Writer {
  public:
    Writer &byte(std::uint8_t value) {
        bytes.push_back(std::byte{value});
        return *this;
    }
    Writer &u16(std::uint16_t value) { return byte(value >> 8).byte(value & 255); }
    Writer &u32(std::uint32_t value) {
        return byte(value >> 24)
            .byte((value >> 16) & 255)
            .byte((value >> 8) & 255)
            .byte(value & 255);
    }
    Writer &raw(ByteView value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
        return *this;
    }
    Writer &text(const std::string &value) {
        if (value.size() > 65535)
            throw std::invalid_argument("MQTT string too long");
        u16(static_cast<std::uint16_t>(value.size()));
        return raw(std::as_bytes(std::span(value.data(), value.size())));
    }
    Bytes bytes;
};

// A fixed header: the type and flags, then the body's length in at most four bytes.
using Header = std::array<std::byte, 5>;

std::size_t header(Header &out, std::uint8_t first, std::size_t length) {
    out[0] = std::byte{first};
    std::size_t size = 1;
    do {
        auto byte = static_cast<std::uint8_t>(length % 128);
        length /= 128;
        if (length > 0)
            byte |= 128;
        out.at(size++) = std::byte{byte};
    } while (length > 0);
    return size;
}

// Copies into a vector of the final size: GCC 13 warns, wrongly, about inserting a range.
Bytes packet(MqttType type, std::uint8_t flags, const Writer &body) {
    Header head{};
    const auto size =
        header(head, static_cast<std::uint8_t>(static_cast<std::uint8_t>(type) << 4 | flags),
               body.bytes.size());
    Bytes out(size + body.bytes.size());
    std::copy_n(head.begin(), size, out.begin());
    std::copy(body.bytes.begin(), body.bytes.end(),
              out.begin() + static_cast<std::ptrdiff_t>(size));
    return out;
}

class Reader {
  public:
    explicit Reader(ByteView bytes) : bytes_(bytes) {}
    std::uint8_t byte() {
        if (at_ >= bytes_.size())
            throw std::out_of_range("short MQTT packet");
        return std::to_integer<std::uint8_t>(bytes_[at_++]);
    }
    std::uint16_t u16() {
        const auto high = byte();
        return static_cast<std::uint16_t>(high << 8 | byte());
    }
    std::size_t varint() {
        std::size_t value = 0;
        for (std::size_t shift = 1, count = 0;; shift *= 128) {
            const auto next = byte();
            value += (next & 127U) * shift;
            if (!(next & 128U))
                return value;
            if (++count == 4)
                throw std::out_of_range("bad MQTT length");
        }
    }
    std::string text() {
        const auto length = u16();
        if (length > bytes_.size() - at_)
            throw std::out_of_range("short MQTT string");
        std::string value(reinterpret_cast<const char *>(bytes_.data() + at_), length);
        at_ += length;
        return value;
    }
    void skip(std::size_t length) {
        if (length > bytes_.size() - at_)
            throw std::out_of_range("short MQTT packet");
        at_ += length;
    }
    [[nodiscard]] ByteView rest() const { return bytes_.subspan(at_); }

  private:
    ByteView bytes_;
    std::size_t at_ = 0;
};

} // namespace

Bytes mqtt_connect(const std::string &client_id, std::uint16_t keep_alive_seconds) {
    Writer body;
    body.text("MQTT").byte(5).byte(0x02).u16(keep_alive_seconds).byte(0).text(client_id);
    return packet(MqttType::Connect, 0, body);
}

Bytes mqtt_publish(const std::string &topic, std::uint16_t packet_id, ByteView payload,
                   std::uint32_t expiry_seconds) {
    Writer body;
    body.text(topic).u16(packet_id).byte(5).byte(message_expiry).u32(expiry_seconds).raw(payload);
    return packet(MqttType::Publish, 0x03, body);
}

Bytes mqtt_subscribe(std::uint16_t packet_id, const std::string &filter, bool replay_retained) {
    Writer body;
    body.u16(packet_id).byte(0).text(filter).byte(replay_retained ? 0x00 : 0x20);
    return packet(MqttType::Subscribe, 0x02, body);
}

Bytes mqtt_unsubscribe(std::uint16_t packet_id, const std::string &filter) {
    Writer body;
    body.u16(packet_id).byte(0).text(filter);
    return packet(MqttType::Unsubscribe, 0x02, body);
}

Bytes mqtt_ping() { return packet(MqttType::Pingreq, 0, Writer{}); }

Bytes mqtt_puback(std::uint16_t packet_id) {
    Writer body;
    body.u16(packet_id);
    return packet(MqttType::Puback, 0, body);
}

std::optional<std::vector<MqttPacket>> MqttDecoder::push(ByteView chunk) {
    if (failed_)
        return std::nullopt;
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    std::vector<MqttPacket> packets;
    std::size_t start = 0;
    while (buffer_.size() - start >= 2) {
        std::size_t length = 0, at = start + 1, shift = 1;
        bool complete = false;
        while (at < buffer_.size() && at - start <= 4) {
            const auto byte = std::to_integer<std::size_t>(buffer_[at++]);
            length += (byte & 127) * shift;
            shift *= 128;
            if (!(byte & 128)) {
                complete = true;
                break;
            }
        }
        if (!complete && at - start > 4)
            failed_ = true;
        if (complete && length > max_)
            failed_ = true;
        if (failed_)
            return std::nullopt;
        if (!complete || buffer_.size() - at < length)
            break;
        const auto first = std::to_integer<std::uint8_t>(buffer_[start]);
        packets.push_back({static_cast<std::uint8_t>(first >> 4),
                           static_cast<std::uint8_t>(first & 15),
                           Bytes(buffer_.begin() + static_cast<std::ptrdiff_t>(at),
                                 buffer_.begin() + static_cast<std::ptrdiff_t>(at + length))});
        start = at + length;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(start));
    return packets;
}

std::optional<MqttAck> mqtt_ack(std::uint8_t type, ByteView body) {
    try {
        Reader reader(body);
        if (type == static_cast<std::uint8_t>(MqttType::Connack)) {
            reader.byte(); // acknowledge flags
            return MqttAck{0, reader.byte()};
        }
        MqttAck ack{reader.u16(), 0};
        if (type == static_cast<std::uint8_t>(MqttType::Puback)) {
            ack.reason = body.size() > 2 ? reader.byte() : 0;
            return ack;
        }
        reader.skip(reader.varint()); // properties
        ack.reason = reader.byte();
        return ack;
    } catch (const std::out_of_range &) {
        return std::nullopt;
    }
}

std::optional<MqttMessage> mqtt_message(std::uint8_t flags, ByteView body) {
    try {
        Reader reader(body);
        MqttMessage message;
        message.topic = reader.text();
        if ((flags >> 1 & 3) > 0)
            message.packet_id = reader.u16();
        reader.skip(reader.varint()); // properties
        message.retained = (flags & 1) == 1;
        const auto payload = reader.rest();
        message.payload.assign(payload.begin(), payload.end());
        return message;
    } catch (const std::out_of_range &) {
        return std::nullopt;
    }
}

BackendFailure mqtt_refusal(std::uint8_t reason) {
    return reason == 0x89 || reason == 0x97 || reason == 0x9f ? BackendFailure::Transient
                                                              : BackendFailure::Rejected;
}

} // namespace p2pdc::detail
