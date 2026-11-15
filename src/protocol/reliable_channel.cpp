#include "reliable_channel.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

struct ReliableChannel::Impl : std::enable_shared_from_this<Impl> {
    ReliableSender sender;
    ReliableReceiver receiver;
    ReliablePump pump;
    Executor executor;
    ReliableChannelCallbacks callbacks;
    bool ready = false;
    bool stopped = false;
    bool scheduled = false;
    bool waiting_writable = false;
    bool writable_scheduled = false;
    std::size_t waiting_size = 0;

    Impl(ReliableChannelLimits limits, MemoryBudget budget, ReliableSink &sink, Executor dispatch,
         ReliableChannelCallbacks handlers)
        : sender(limits.frames, budget, limits.send_bytes, limits.send_messages),
          receiver(limits.frames, std::move(budget)),
          pump(sender, sink, limits.backend_high_water, limits.frames_per_pass),
          executor(std::move(dispatch)), callbacks(std::move(handlers)) {
        const auto payload = std::min(limits.frames.max_frame_size - frame_header_size,
                                      static_cast<std::size_t>(limits.frames.max_message_size));
        if (frame_header_size + payload > limits.backend_high_water) {
            throw std::invalid_argument("backend high water cannot hold a maximum frame");
        }
    }

    void stop() {
        stopped = true;
        ready = false;
        sender.clear();
        receiver.reset();
    }

    void fail(ChannelFailure error) {
        if (!stopped) {
            stop();
            callbacks.on_failure(error);
        }
    }

    void schedule() {
        if (stopped || !ready || scheduled) {
            return;
        }
        scheduled = true;
        executor([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->run();
            }
        });
    }

    void notify_writable() {
        if (stopped || !ready || !waiting_writable || writable_scheduled ||
            !sender.can_enqueue(waiting_size)) {
            return;
        }
        writable_scheduled = true;
        executor([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->writable_scheduled = false;
                if (self->ready && self->waiting_writable &&
                    self->sender.can_enqueue(self->waiting_size)) {
                    self->waiting_writable = false;
                    self->callbacks.on_writable();
                }
            }
        });
    }

    void run() {
        scheduled = false;
        if (!ready || stopped) {
            return;
        }
        const auto result = pump.run();
        if (result == PumpResult::Failed) {
            fail(ChannelFailure::Transport);
            return;
        }
        notify_writable();
        if (!stopped && result == PumpResult::More) {
            schedule();
        }
    }

    ChannelSend send(ByteView message) {
        if (stopped) {
            return ChannelSend::Closed;
        }
        if (!ready) {
            return ChannelSend::NotReady;
        }
        switch (sender.enqueue(message)) {
        case SenderEnqueue::Accepted:
            schedule();
            return ChannelSend::Accepted;
        case SenderEnqueue::TooLarge:
            return ChannelSend::TooLarge;
        case SenderEnqueue::Backpressure:
            waiting_writable = true;
            waiting_size = message.size();
            return ChannelSend::Backpressure;
        case SenderEnqueue::AllocationFailed:
            return ChannelSend::Failed;
        case SenderEnqueue::Closed:
            return ChannelSend::Closed;
        }
        return ChannelSend::Failed;
    }

    void receive(ByteView frame) {
        if (stopped) {
            return;
        }
        if (!ready) {
            fail(ChannelFailure::Transport);
            return;
        }
        auto result = receiver.push(frame);
        if (result.error) {
            const auto error = *result.error;
            fail(error == ReceiveError::InvalidFrame    ? ChannelFailure::InvalidFrame
                 : error == ReceiveError::ResourceLimit ? ChannelFailure::ReceiveLimit
                                                        : ChannelFailure::Allocation);
        } else if (result.message) {
            callbacks.on_message(std::move(result.message->bytes));
            result.message.reset();
            notify_writable();
        }
    }
};

ReliableChannel::ReliableChannel(ReliableChannelLimits limits, MemoryBudget budget,
                                 ReliableSink &sink, Executor executor,
                                 ReliableChannelCallbacks callbacks) {
    if (!executor || !callbacks.on_message || !callbacks.on_failure || !callbacks.on_writable) {
        throw std::invalid_argument("reliable channel requires executor and callbacks");
    }
    impl_ = std::make_shared<Impl>(limits, std::move(budget), sink, std::move(executor),
                                   std::move(callbacks));
}

ReliableChannel::~ReliableChannel() { close(); }
ChannelSend ReliableChannel::send(ByteView message) { return impl_->send(message); }
void ReliableChannel::set_ready(bool ready) {
    if (!impl_->stopped) {
        impl_->ready = ready;
        impl_->notify_writable();
        impl_->schedule();
    }
}
void ReliableChannel::on_frame(ByteView frame) {
    auto keep_alive = impl_;
    keep_alive->receive(frame);
}
void ReliableChannel::wake() {
    auto keep_alive = impl_;
    if (keep_alive->stopped || !keep_alive->ready) {
        return;
    }
    keep_alive->notify_writable();
    keep_alive->schedule();
}
void ReliableChannel::close() {
    if (impl_ && !impl_->stopped) {
        impl_->stop();
    }
}

} // namespace p2pdc::detail
