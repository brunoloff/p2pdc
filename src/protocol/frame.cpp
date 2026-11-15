#include "p2pdc/frame.h"

#include <optional>

namespace p2pdc {
namespace {
constexpr std::byte magic_first{0x50};
constexpr std::byte magic_second{0x44};
constexpr std::byte version{0x01};

void put_integer(Bytes &out, std::uint64_t value, unsigned int size) {
    for (unsigned int i = size; i > 0; --i) {
        out.push_back(static_cast<std::byte>((value >> ((i - 1U) * 8U)) & 0xffU));
    }
}

std::uint64_t read_integer(ByteView bytes, std::size_t start, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[start + i]);
    }
    return value;
}

std::optional<FrameError> validate(std::uint32_t total, std::uint32_t offset,
                                   std::size_t payload_size, FrameLimits limits) {
    if (limits.max_frame_size < frame_header_size || total > limits.max_message_size ||
        payload_size > limits.max_frame_size - frame_header_size) {
        return FrameError::TooLarge;
    }
    if (offset > total || payload_size > total - offset) {
        return FrameError::InvalidBounds;
    }
    if (total > 0 && payload_size == 0) {
        return FrameError::EmptyFragment;
    }
    return std::nullopt;
}
} // namespace

std::variant<Bytes, FrameError> encode_frame(const Frame &frame, FrameLimits limits) {
    return encode_fragment(frame.message_id, frame.total_size, frame.offset, frame.payload, limits);
}

std::variant<Bytes, FrameError> encode_fragment(std::uint64_t message_id, std::uint32_t total_size,
                                                std::uint32_t offset, ByteView payload,
                                                FrameLimits limits) {
    if (const auto error = validate(total_size, offset, payload.size(), limits)) {
        return *error;
    }
    Bytes result;
    result.reserve(frame_header_size + payload.size());
    result.insert(result.end(), {magic_first, magic_second, version});
    put_integer(result, message_id, 8);
    put_integer(result, total_size, 4);
    put_integer(result, offset, 4);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

std::variant<FrameView, FrameError> decode_frame_view(ByteView bytes, FrameLimits limits) {
    if (bytes.size() < frame_header_size) {
        return FrameError::Truncated;
    }
    if (bytes.size() > limits.max_frame_size) {
        return FrameError::TooLarge;
    }
    if (bytes[0] != magic_first || bytes[1] != magic_second) {
        return FrameError::InvalidMagic;
    }
    if (bytes[2] != version) {
        return FrameError::InvalidVersion;
    }
    FrameView frame{
        read_integer(bytes, 3, 8), static_cast<std::uint32_t>(read_integer(bytes, 11, 4)),
        static_cast<std::uint32_t>(read_integer(bytes, 15, 4)), bytes.subspan(frame_header_size)};
    if (const auto error =
            validate(frame.total_size, frame.offset, bytes.size() - frame_header_size, limits)) {
        return *error;
    }
    return frame;
}

std::variant<Frame, FrameError> decode_frame(ByteView bytes, FrameLimits limits) {
    auto decoded = decode_frame_view(bytes, limits);
    if (auto *error = std::get_if<FrameError>(&decoded)) {
        return *error;
    }
    const auto &frame = std::get<FrameView>(decoded);
    return Frame{frame.message_id, frame.total_size, frame.offset,
                 Bytes(frame.payload.begin(), frame.payload.end())};
}

} // namespace p2pdc
