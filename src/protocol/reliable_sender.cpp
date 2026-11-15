#include "reliable_sender.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

ReliableSender::ReliableSender(FrameLimits limits, MemoryBudget budget,
                               std::size_t max_queued_bytes, std::size_t max_messages)
    : limits_(limits), budget_(std::move(budget)), max_queued_bytes_(max_queued_bytes),
      max_messages_(max_messages) {
    if (limits.max_frame_size <= frame_header_size || max_messages == 0 ||
        limits.max_message_size > max_queued_bytes) {
        throw std::invalid_argument("invalid reliable sender limits");
    }
}

SenderEnqueue ReliableSender::enqueue(ByteView message) {
    if (closed_) {
        return SenderEnqueue::Closed;
    }
    if (message.size() > limits_.max_message_size) {
        return SenderEnqueue::TooLarge;
    }
    if (messages_.size() >= max_messages_ || message.size() > max_queued_bytes_ - queued_bytes_) {
        return SenderEnqueue::Backpressure;
    }
    std::optional<MemoryReservation> scratch;
    if (messages_.empty()) {
        const auto payload = std::min(limits_.max_frame_size - frame_header_size,
                                      static_cast<std::size_t>(limits_.max_message_size));
        scratch = budget_.try_reserve(frame_header_size + payload);
        if (!scratch) {
            return SenderEnqueue::Backpressure;
        }
    }
    auto reservation = budget_.try_reserve(message.size());
    if (!reservation) {
        return SenderEnqueue::Backpressure;
    }
    return admit(message, std::move(*reservation), std::move(scratch));
}

bool ReliableSender::can_enqueue(std::size_t size) const {
    if (closed_ || size > limits_.max_message_size || messages_.size() >= max_messages_ ||
        size > max_queued_bytes_ - queued_bytes_) {
        return false;
    }
    const auto used = budget_.used();
    const auto available = budget_.limit() - used;
    const auto payload = std::min(limits_.max_frame_size - frame_header_size,
                                  static_cast<std::size_t>(limits_.max_message_size));
    const auto scratch = messages_.empty() ? frame_header_size + payload : 0;
    return scratch <= available && size <= available - scratch;
}

SenderEnqueue ReliableSender::admit(ByteView message, MemoryReservation reservation,
                                    std::optional<MemoryReservation> scratch) {
    try {
        Bytes copy(message.begin(), message.end());
        std::optional<MemoryReservation> extra;
        if (copy.capacity() > reservation.bytes()) {
            extra = budget_.try_reserve(copy.capacity() - reservation.bytes());
            if (!extra) {
                return SenderEnqueue::Backpressure;
            }
        }
        messages_.push_back({std::move(copy), std::move(reservation), std::move(extra)});
        if (scratch) {
            frame_reservation_ = std::move(scratch);
        }
        queued_bytes_ += message.size();
        return SenderEnqueue::Accepted;
    } catch (const std::bad_alloc &) {
        return SenderEnqueue::AllocationFailed;
    } catch (const std::length_error &) {
        return SenderEnqueue::AllocationFailed;
    }
}

SenderFrame ReliableSender::peek() {
    if (frame_) {
        return {ByteView(*frame_), std::nullopt};
    }
    if (messages_.empty()) {
        return {};
    }
    const auto &message = messages_.front().bytes;
    const auto length =
        std::min(message.size() - offset_, limits_.max_frame_size - frame_header_size);
    try {
        auto encoded = encode_fragment(message_id_, static_cast<std::uint32_t>(message.size()),
                                       static_cast<std::uint32_t>(offset_),
                                       ByteView(message).subspan(offset_, length), limits_);
        if (std::holds_alternative<FrameError>(encoded)) {
            return {std::nullopt, SenderFrameError::InvalidFrame};
        }
        auto bytes = std::move(std::get<Bytes>(encoded));
        if (bytes.capacity() > frame_reservation_->bytes()) {
            frame_extra_ = budget_.try_reserve(bytes.capacity() - frame_reservation_->bytes());
            if (!frame_extra_) {
                return {std::nullopt, SenderFrameError::Backpressure};
            }
        }
        frame_ = std::move(bytes);
        return {ByteView(*frame_), std::nullopt};
    } catch (const std::bad_alloc &) {
        return {std::nullopt, SenderFrameError::AllocationFailed};
    } catch (const std::length_error &) {
        return {std::nullopt, SenderFrameError::AllocationFailed};
    }
}

void ReliableSender::commit() {
    if (!frame_) {
        throw std::logic_error("no pending frame to commit");
    }
    offset_ += frame_->size() - frame_header_size;
    frame_.reset();
    frame_extra_.reset();
    if (offset_ == messages_.front().bytes.size()) {
        queued_bytes_ -= messages_.front().bytes.size();
        messages_.pop_front();
        offset_ = 0;
        ++message_id_;
        if (messages_.empty()) {
            frame_reservation_.reset();
        }
    }
}

void ReliableSender::clear() {
    closed_ = true;
    frame_.reset();
    frame_reservation_.reset();
    frame_extra_.reset();
    messages_.clear();
    queued_bytes_ = 0;
    offset_ = 0;
}

std::size_t ReliableSender::queued_bytes() const { return queued_bytes_; }
std::size_t ReliableSender::queued_messages() const { return messages_.size(); }

} // namespace p2pdc::detail
