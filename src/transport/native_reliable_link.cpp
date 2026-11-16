#include "native_reliable_link.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

ConnectError channel_failure_category(ChannelFailure failure) {
    switch (failure) {
    case ChannelFailure::Transport:
        return ConnectError::Transport;
    case ChannelFailure::InvalidFrame:
        return ConnectError::Protocol;
    case ChannelFailure::ReceiveLimit:
    case ChannelFailure::Allocation:
        return ConnectError::ResourceLimit;
    }
    return ConnectError::Backend;
}

struct NativeReliableLink::Impl : ReliableSink, std::enable_shared_from_this<Impl> {
    Executor executor;
    NativeLinkCallbacks callbacks;
    ReliableChannelLimits limits;
    MemoryBudget budget;
    std::unique_ptr<NativeTransport> transport;
    std::unique_ptr<ReliableChannel> channel;
    std::optional<std::size_t> receive_limit;
    NativeTransportState status = NativeTransportState::Connecting;
    bool stopped = false;
    bool owner_alive = true;
    bool negotiated = false;

    Impl(Executor dispatch, NativeLinkCallbacks handlers, ReliableChannelLimits config,
         MemoryBudget shared_budget)
        : executor(std::move(dispatch)), callbacks(std::move(handlers)), limits(config),
          budget(std::move(shared_budget)) {}

    std::size_t buffered_amount() const override { return transport->buffered_amount(); }
    SinkResult send(ByteView frame) override {
        try {
            switch (transport->send(frame)) {
            case NativeSendResult::Accepted:
                return SinkResult::Accepted;
            case NativeSendResult::NotReady:
                return SinkResult::NotReady;
            default:
                return SinkResult::Failed;
            }
        } catch (const std::exception &) {
            return SinkResult::Failed;
        }
    }

    void stop() {
        stopped = true;
        if (channel) {
            channel->close();
        }
        if (transport) {
            transport->close();
        }
    }

    void fail(std::string reason, ConnectError category = ConnectError::Transport) {
        if (stopped) {
            return;
        }
        status = NativeTransportState::Failed;
        stop();
        callbacks.on_error(category, std::move(reason));
        if (owner_alive) {
            callbacks.on_state(status);
        }
        if (owner_alive && callbacks.on_route) {
            callbacks.on_route(transport->poll_route());
        }
    }

    void change_state(NativeTransportState next) {
        if (next == NativeTransportState::Failed) {
            fail("Native transport failed");
            return;
        }
        if (next == NativeTransportState::Closed) {
            status = next;
            stop();
            callbacks.on_state(status);
            if (owner_alive && callbacks.on_route) {
                callbacks.on_route(transport->poll_route());
            }
            return;
        }
        if (next == NativeTransportState::Ready && !configure_negotiated_limit()) {
            return;
        }
        status = next;
        channel->set_ready(next == NativeTransportState::Ready);
        callbacks.on_state(status);
    }

    bool configure_negotiated_limit() {
        const auto backend_max = transport->max_message_size();
        const auto payload = std::min(limits.frames.max_frame_size - frame_header_size,
                                      static_cast<std::size_t>(limits.frames.max_message_size));
        if (backend_max <= frame_header_size ||
            (negotiated && backend_max < frame_header_size + payload)) {
            fail("Negotiated data-channel limit cannot carry configured frames");
            return false;
        }
        if (!negotiated && backend_max < limits.frames.max_frame_size) {
            limits.frames.max_frame_size = backend_max;
            try {
                channel = std::make_unique<ReliableChannel>(limits, budget, *this, executor,
                                                            channel_callbacks());
                if (receive_limit)
                    channel->set_receive_limit(*receive_limit);
            } catch (const std::exception &error) {
                fail(std::string("Cannot configure negotiated frame size: ") + error.what());
                return false;
            }
        }
        negotiated = true;
        return true;
    }

    NativeTransportCallbacks native_callbacks() {
        auto weak = weak_from_this();
        NativeTransportCallbacks out;
        out.on_description = [weak](rtc::Description value) {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->callbacks.on_description(std::move(value));
            }
        };
        out.on_candidate = [weak](rtc::Candidate value) {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->callbacks.on_candidate(std::move(value));
            }
        };
        add_transport_events(out);
        return out;
    }

    void add_transport_events(NativeTransportCallbacks &out) {
        auto weak = weak_from_this();
        out.on_state = [weak](NativeTransportState value) {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->change_state(value);
            }
        };
        out.on_binary = [weak](Bytes value) {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->channel->on_frame(value);
            }
        };
        out.on_route = [weak](RouteInfo value) {
            if (auto self = weak.lock(); self && !self->stopped && self->callbacks.on_route) {
                self->callbacks.on_route(value);
            }
        };
        out.on_error = [weak](ConnectError category, std::string value) {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->fail(std::move(value), category);
            }
        };
        out.on_writable = [weak] {
            if (auto self = weak.lock(); self && !self->stopped) {
                self->channel->wake();
            }
        };
    }

    ReliableChannelCallbacks channel_callbacks() {
        auto weak = weak_from_this();
        return {[weak](Bytes value) {
                    if (auto self = weak.lock(); self && !self->stopped) {
                        self->callbacks.on_message(std::move(value));
                    }
                },
                [weak](ChannelFailure failure) {
                    if (auto self = weak.lock(); self && !self->stopped) {
                        self->fail("Reliable message transport failed",
                                   channel_failure_category(failure));
                    }
                },
                [weak] {
                    if (auto self = weak.lock(); self && !self->stopped) {
                        self->callbacks.on_writable();
                    }
                }};
    }
};

NativeReliableLink::NativeReliableLink(rtc::Configuration config, NativeRelayPolicy policy,
                                       ReliableChannelLimits limits, MemoryBudget budget,
                                       NativeReceiveLimits inbound_limits, Executor executor,
                                       NativeLinkCallbacks callbacks) {
    if (!executor || !callbacks.on_description || !callbacks.on_candidate || !callbacks.on_state ||
        !callbacks.on_message || !callbacks.on_error || !callbacks.on_writable) {
        throw std::invalid_argument("native link requires executor and callbacks");
    }
    if (config.disableFingerprintVerification) {
        throw std::invalid_argument("native link requires DTLS fingerprint verification");
    }
    if (limits.frames.max_frame_size <= frame_header_size) {
        throw std::invalid_argument("native link requires a valid frame size");
    }
    const auto payload = std::min(limits.frames.max_frame_size - frame_header_size,
                                  static_cast<std::size_t>(limits.frames.max_message_size));
    if (frame_header_size + payload > inbound_limits.max_message_size) {
        throw std::invalid_argument("native inbound limit cannot hold a maximum frame");
    }
    const auto advertised_max = frame_header_size + payload;
    if (!config.maxMessageSize || *config.maxMessageSize == 0 ||
        *config.maxMessageSize > advertised_max) {
        config.maxMessageSize = advertised_max;
    }
    impl_ = std::make_shared<Impl>(executor, std::move(callbacks), limits, budget);
    impl_->transport = std::make_unique<NativeTransport>(std::move(config), policy, executor,
                                                         impl_->native_callbacks(), inbound_limits);
    // Wake the pump at a quarter of the high-water mark so it refills the
    // backend buffer before it drains, instead of stop-and-wait.
    impl_->transport->set_writable_threshold(limits.backend_high_water / 4);
    impl_->channel = std::make_unique<ReliableChannel>(limits, std::move(budget), *impl_, executor,
                                                       impl_->channel_callbacks());
}

NativeReliableLink::~NativeReliableLink() { close(); }
void NativeReliableLink::start_offer() { impl_->transport->start_offer(); }
void NativeReliableLink::set_remote_description(rtc::Description value) {
    impl_->transport->set_remote_description(std::move(value));
}
void NativeReliableLink::add_remote_candidate(rtc::Candidate value) {
    impl_->transport->add_remote_candidate(std::move(value));
}
ChannelSend NativeReliableLink::send(ByteView message) { return impl_->channel->send(message); }
std::size_t NativeReliableLink::writable_threshold() const {
    return impl_->transport->writable_threshold();
}
void NativeReliableLink::set_receive_limit(std::size_t bytes) {
    impl_->receive_limit = bytes;
    impl_->channel->set_receive_limit(bytes);
}
NativeTransportState NativeReliableLink::state() const { return impl_->status; }
RouteInfo NativeReliableLink::poll_route() { return impl_->transport->poll_route(); }
void NativeReliableLink::wake() { impl_->channel->wake(); }
void NativeReliableLink::close() {
    if (impl_) {
        impl_->owner_alive = false;
        if (!impl_->stopped) {
            impl_->status = NativeTransportState::Closed;
            impl_->stop();
        }
    }
}

} // namespace p2pdc::detail
