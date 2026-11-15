#include "p2pdc/reassembler.h"

#include <utility>

namespace p2pdc {

Reassembler::Reassembler(FrameLimits limits) : limits_(limits) {}

ReassemblyResult Reassembler::push(ByteView bytes) {
    auto decoded = decode_frame_view(bytes, limits_);
    if (const auto *error = std::get_if<FrameError>(&decoded)) {
        reset();
        return {std::nullopt, *error};
    }
    auto &frame = std::get<FrameView>(decoded);
    if (!message_id_) {
        if (frame.offset != 0) {
            return {std::nullopt, FrameError::UnexpectedFrame};
        }
        message_id_ = frame.message_id;
        total_size_ = frame.total_size;
        buffer_.reserve(total_size_);
    }
    if (*message_id_ != frame.message_id || total_size_ != frame.total_size ||
        buffer_.size() != frame.offset) {
        reset();
        return {std::nullopt, FrameError::UnexpectedFrame};
    }
    buffer_.insert(buffer_.end(), frame.payload.begin(), frame.payload.end());
    if (buffer_.size() == total_size_) {
        Bytes message = std::move(buffer_);
        reset();
        return {std::move(message), std::nullopt};
    }
    return {};
}

void Reassembler::reset() {
    message_id_.reset();
    total_size_ = 0;
    Bytes{}.swap(buffer_);
}

std::size_t Reassembler::buffered_bytes() const { return buffer_.size(); }

std::size_t Reassembler::allocated_bytes() const { return buffer_.capacity(); }

} // namespace p2pdc
