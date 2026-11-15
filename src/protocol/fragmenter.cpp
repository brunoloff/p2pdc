#include "p2pdc/fragmenter.h"

#include <algorithm>
#include <utility>

namespace p2pdc {

Fragmenter::Fragmenter(Bytes message, std::uint64_t message_id, FrameLimits limits)
    : message_(std::move(message)), message_id_(message_id), limits_(limits) {}

NextFrame Fragmenter::next() {
    if (finished_) {
        return {};
    }
    if (message_.size() > limits_.max_message_size || limits_.max_frame_size < frame_header_size ||
        (!message_.empty() && limits_.max_frame_size == frame_header_size)) {
        finished_ = true;
        Bytes{}.swap(message_);
        return {std::nullopt, FrameError::TooLarge};
    }
    const auto count =
        std::min(message_.size() - offset_, limits_.max_frame_size - frame_header_size);
    auto encoded = encode_fragment(message_id_, static_cast<std::uint32_t>(message_.size()),
                                   static_cast<std::uint32_t>(offset_),
                                   ByteView(message_).subspan(offset_, count), limits_);
    if (const auto *error = std::get_if<FrameError>(&encoded)) {
        finished_ = true;
        Bytes{}.swap(message_);
        return {std::nullopt, *error};
    }
    offset_ += count;
    finished_ = offset_ == message_.size();
    if (finished_) {
        Bytes{}.swap(message_);
    }
    return {std::move(std::get<Bytes>(encoded)), std::nullopt};
}

std::size_t Fragmenter::allocated_bytes() const { return message_.capacity(); }

} // namespace p2pdc
