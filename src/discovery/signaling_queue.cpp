#include "signaling_queue.hpp"

#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

SignalingQueue::SignalingQueue(MemoryBudget budget, std::size_t max_bytes, std::size_t max_records,
                               std::size_t max_record_bytes)
    : budget_(std::move(budget)), max_bytes_(max_bytes), max_records_(max_records),
      max_record_bytes_(max_record_bytes) {
    if (max_records == 0 || max_record_bytes > max_bytes) {
        throw std::invalid_argument("invalid signaling queue limits");
    }
}

SignalingQueueResult SignalingQueue::push(Bytes record) {
    if (closed_) {
        return SignalingQueueResult::Closed;
    }
    if (record.size() > max_record_bytes_) {
        return SignalingQueueResult::TooLarge;
    }
    const auto capacity = record.capacity();
    if (records_.size() >= max_records_ || capacity > max_bytes_ - bytes_) {
        return SignalingQueueResult::Full;
    }
    auto reservation = budget_.try_reserve(capacity);
    if (!reservation) {
        return SignalingQueueResult::Full;
    }
    try {
        records_.push_back({std::move(record), std::move(*reservation)});
    } catch (const std::bad_alloc &) {
        return SignalingQueueResult::AllocationFailed;
    } catch (const std::length_error &) {
        return SignalingQueueResult::AllocationFailed;
    }
    bytes_ += capacity;
    return SignalingQueueResult::Accepted;
}

std::optional<ByteView> SignalingQueue::front() const {
    if (records_.empty()) {
        return std::nullopt;
    }
    return ByteView(records_.front().data);
}

bool SignalingQueue::pop() {
    if (records_.empty()) {
        return false;
    }
    bytes_ -= records_.front().reservation.bytes();
    records_.pop_front();
    return true;
}

void SignalingQueue::close() {
    closed_ = true;
    records_.clear();
    bytes_ = 0;
}

std::size_t SignalingQueue::size() const { return records_.size(); }
std::size_t SignalingQueue::buffered_bytes() const { return bytes_; }

} // namespace p2pdc::detail
