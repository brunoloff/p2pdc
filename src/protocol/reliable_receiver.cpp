#include "reliable_receiver.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

ReliableReceiver::ReliableReceiver(FrameLimits limits, MemoryBudget budget)
    : limits_(limits), budget_(std::move(budget)), reassembler_(limits),
      message_limit_(limits.max_message_size) {}

void ReliableReceiver::set_message_limit(std::size_t bytes) {
    message_limit_ = std::min<std::size_t>(bytes, limits_.max_message_size);
}

void ReliableReceiver::reset() {
    reassembler_.reset();
    reservation_.reset();
    extra_.reset();
}

ReceiveResult ReliableReceiver::fail(ReceiveError error, std::optional<FrameError> frame_error) {
    reset();
    return {std::nullopt, error, frame_error};
}

ReceiveResult ReliableReceiver::push(ByteView bytes) {
    try {
        auto decoded = decode_frame_view(bytes, limits_);
        if (auto *error = std::get_if<FrameError>(&decoded)) {
            return fail(ReceiveError::InvalidFrame, *error);
        }
        return push_validated(bytes, std::get<FrameView>(decoded));
    } catch (const std::bad_alloc &) {
        return fail(ReceiveError::AllocationFailed);
    } catch (const std::length_error &) {
        return fail(ReceiveError::AllocationFailed);
    }
}

ReceiveResult ReliableReceiver::push_validated(ByteView bytes, const FrameView &frame) {
    if (!reservation_) {
        if (frame.offset != 0) {
            return fail(ReceiveError::InvalidFrame, FrameError::UnexpectedFrame);
        }
        if (frame.total_size > message_limit_) {
            return fail(ReceiveError::ResourceLimit);
        }
        reservation_ = budget_.try_reserve(frame.total_size);
        if (!reservation_) {
            return fail(ReceiveError::ResourceLimit);
        }
    }
    auto result = reassembler_.push(bytes);
    if (result.error) {
        return fail(ReceiveError::InvalidFrame, *result.error);
    }
    const auto capacity =
        result.message ? result.message->capacity() : reassembler_.allocated_bytes();
    if (capacity > reservation_->bytes() && !extra_) {
        extra_ = budget_.try_reserve(capacity - reservation_->bytes());
        if (!extra_) {
            return fail(ReceiveError::ResourceLimit);
        }
    }
    if (!result.message) {
        return {};
    }
    ReservedMessage complete{std::move(*result.message), std::move(*reservation_),
                             std::move(extra_)};
    reservation_.reset();
    extra_.reset();
    return {std::move(complete), std::nullopt, std::nullopt};
}

} // namespace p2pdc::detail
