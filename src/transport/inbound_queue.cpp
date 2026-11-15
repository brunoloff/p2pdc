#include "inbound_queue.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

InboundQueue::InboundQueue(MemoryBudget budget, std::size_t max_bytes, std::size_t max_messages,
                           std::size_t max_message_size)
    : budget_(std::move(budget)), max_bytes_(max_bytes), max_messages_(max_messages),
      max_message_size_(max_message_size) {
    if (max_messages == 0 || max_message_size > max_bytes) {
        throw std::invalid_argument("invalid inbound queue limits");
    }
}

InboundPush InboundQueue::fail(InboundStatus error) {
    messages_.clear();
    bytes_ = 0;
    error_ = error;
    const bool schedule = !draining_;
    draining_ = true;
    return {error, schedule};
}

InboundPush InboundQueue::push(Bytes &&message, bool deduplicate_content) {
    std::lock_guard lock(mutex_);
    if (closed_ || error_) {
        return {InboundStatus::Closed, false};
    }
    if (message.size() > max_message_size_) {
        return fail(InboundStatus::TooLarge);
    }
    if (deduplicate_content &&
        std::any_of(messages_.begin(), messages_.end(),
                    [&](const InboundMessage &queued) { return queued.payload == message; }))
        return {InboundStatus::Accepted, false};
    const auto capacity = message.capacity();
    if (messages_.size() >= max_messages_ || capacity > max_bytes_ - bytes_) {
        return fail(InboundStatus::Full);
    }
    auto reservation = budget_.try_reserve(capacity);
    if (!reservation) {
        return fail(InboundStatus::Full);
    }
    try {
        messages_.push_back({std::move(message), std::move(*reservation)});
    } catch (const std::bad_alloc &) {
        return fail(InboundStatus::AllocationFailed);
    } catch (const std::length_error &) {
        return fail(InboundStatus::AllocationFailed);
    }
    bytes_ += capacity;
    const bool schedule = !draining_;
    draining_ = true;
    return {InboundStatus::Accepted, schedule};
}

InboundPush InboundQueue::reject_invalid_type() {
    std::lock_guard lock(mutex_);
    if (closed_ || error_) {
        return {InboundStatus::Closed, false};
    }
    return fail(InboundStatus::InvalidType);
}

InboundNext InboundQueue::next() {
    std::lock_guard lock(mutex_);
    if (error_) {
        const auto error = *error_;
        error_.reset();
        closed_ = true;
        draining_ = false;
        return error;
    }
    if (closed_) {
        return InboundStatus::Closed;
    }
    if (messages_.empty()) {
        draining_ = false;
        return InboundStatus::Idle;
    }
    InboundMessage message = std::move(messages_.front());
    messages_.pop_front();
    bytes_ -= message.payload.capacity();
    return message;
}

void InboundQueue::close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    error_.reset();
    messages_.clear();
    bytes_ = 0;
    draining_ = false;
}

std::size_t InboundQueue::buffered_bytes() const {
    std::lock_guard lock(mutex_);
    return bytes_;
}

} // namespace p2pdc::detail
