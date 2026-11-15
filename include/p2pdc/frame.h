#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace p2pdc {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

constexpr std::size_t frame_header_size = 19;

enum class FrameError {
    Truncated,
    InvalidMagic,
    InvalidVersion,
    InvalidBounds,
    EmptyFragment,
    TooLarge,
    UnexpectedFrame
};

struct FrameLimits {
    std::uint32_t max_message_size;
    std::size_t max_frame_size;
};

struct Frame {
    std::uint64_t message_id;
    std::uint32_t total_size;
    std::uint32_t offset;
    Bytes payload;
};

struct FrameView {
    std::uint64_t message_id;
    std::uint32_t total_size;
    std::uint32_t offset;
    ByteView payload;
};

[[nodiscard]] std::variant<Bytes, FrameError> encode_frame(const Frame &frame, FrameLimits limits);
[[nodiscard]] std::variant<Bytes, FrameError> encode_fragment(std::uint64_t message_id,
                                                              std::uint32_t total_size,
                                                              std::uint32_t offset,
                                                              ByteView payload, FrameLimits limits);
[[nodiscard]] std::variant<Frame, FrameError> decode_frame(ByteView bytes, FrameLimits limits);
// The returned payload borrows from bytes and cannot outlive its storage.
[[nodiscard]] std::variant<FrameView, FrameError> decode_frame_view(ByteView bytes,
                                                                    FrameLimits limits);

} // namespace p2pdc
