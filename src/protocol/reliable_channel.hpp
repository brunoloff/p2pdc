#pragma once

#include "p2pdc/operation.h"
#include "reliable_pump.hpp"
#include "reliable_receiver.hpp"

namespace p2pdc::detail {

enum class ChannelSend { Accepted, NotReady, TooLarge, Backpressure, Failed, Closed };
enum class ChannelFailure { Transport, InvalidFrame, ReceiveLimit, Allocation };

struct ReliableChannelLimits {
    FrameLimits frames;
    std::size_t send_bytes;
    std::size_t send_messages;
    std::size_t backend_high_water;
    std::size_t frames_per_pass;
};

struct ReliableChannelCallbacks {
    std::function<void(Bytes)> on_message;
    std::function<void(ChannelFailure)> on_failure;
    std::function<void()> on_writable;
};

// Internal connection component, not an authenticated connection. All calls,
// destruction and callbacks run on one serialized FIFO executor. The executor
// must post asynchronously, not throw, and outlive the channel. The sink must
// outlive it and obey ReliableSink's non-reentrancy contract. User callbacks
// must not throw. on_frame is called only from an already-posted backend event.
// wake() must be called on transport recovery, buffer-low and shared-budget
// availability events. close() cancels queued work without delivering callbacks.
class ReliableChannel {
  public:
    ReliableChannel(ReliableChannelLimits limits, MemoryBudget budget, ReliableSink &sink,
                    Executor executor, ReliableChannelCallbacks callbacks);
    ~ReliableChannel();
    ReliableChannel(const ReliableChannel &) = delete;
    ReliableChannel &operator=(const ReliableChannel &) = delete;
    [[nodiscard]] ChannelSend send(ByteView message);
    void set_ready(bool ready);
    void on_frame(ByteView frame);
    void wake();
    void close();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace p2pdc::detail
