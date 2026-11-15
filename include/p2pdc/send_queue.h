#pragma once

#include "p2pdc/frame.h"

#include <deque>
#include <optional>

namespace p2pdc {

enum class EnqueueResult { Accepted, TooLarge, Backpressure };

class SendQueue {
  public:
    SendQueue(std::size_t max_bytes, std::size_t max_messages, std::size_t max_message_size);
    SendQueue(const SendQueue &) = delete;
    SendQueue &operator=(const SendQueue &) = delete;
    SendQueue(SendQueue &&) = delete;
    SendQueue &operator=(SendQueue &&) = delete;
    [[nodiscard]] EnqueueResult enqueue(ByteView message);
    [[nodiscard]] std::optional<Bytes> pop();
    [[nodiscard]] std::size_t buffered_bytes() const;
    [[nodiscard]] std::size_t size() const;

  private:
    std::size_t max_bytes_;
    std::size_t max_messages_;
    std::size_t max_message_size_;
    std::size_t buffered_bytes_ = 0;
    std::deque<Bytes> messages_;
};

} // namespace p2pdc
