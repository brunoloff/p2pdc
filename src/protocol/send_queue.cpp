#include "p2pdc/send_queue.h"

#include <stdexcept>
#include <utility>

namespace p2pdc {

SendQueue::SendQueue(std::size_t max_bytes, std::size_t max_messages, std::size_t max_message_size)
    : max_bytes_(max_bytes), max_messages_(max_messages), max_message_size_(max_message_size) {
    if (max_messages == 0 || max_message_size > max_bytes) {
        throw std::invalid_argument("send queue limits cannot accommodate a message");
    }
}

EnqueueResult SendQueue::enqueue(ByteView message) {
    if (message.size() > max_message_size_) {
        return EnqueueResult::TooLarge;
    }
    if (messages_.size() == max_messages_ || message.size() > max_bytes_ - buffered_bytes_) {
        return EnqueueResult::Backpressure;
    }
    messages_.emplace_back(message.begin(), message.end());
    buffered_bytes_ += message.size();
    return EnqueueResult::Accepted;
}

std::optional<Bytes> SendQueue::pop() {
    if (messages_.empty()) {
        return std::nullopt;
    }
    Bytes message = std::move(messages_.front());
    messages_.pop_front();
    buffered_bytes_ -= message.size();
    return message;
}

std::size_t SendQueue::buffered_bytes() const { return buffered_bytes_; }

std::size_t SendQueue::size() const { return messages_.size(); }

} // namespace p2pdc
