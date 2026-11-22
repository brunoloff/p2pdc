#include "native_transport_impl.hpp"

namespace p2pdc::detail {
NativeTransport::NativeTransport(rtc::Configuration config, NativeRelayPolicy policy,
                                 Executor executor, NativeTransportCallbacks callbacks,
                                 NativeReceiveLimits limits) {
    if (!executor || !callbacks.on_description || !callbacks.on_candidate || !callbacks.on_binary ||
        !callbacks.on_state || !callbacks.on_error) {
        throw std::invalid_argument(
            "Native transport requires executor and signaling/data/state/error callbacks");
    }
    if (limits.max_message_size == 0) {
        throw std::invalid_argument("Native transport requires a nonzero receive limit");
    }
    impl_ = std::make_shared<Impl>(std::move(config), policy, std::move(executor),
                                   std::move(callbacks), std::move(limits));
    impl_->bind_peer();
}

NativeTransport::~NativeTransport() { close(); }

void NativeTransport::start_offer() {
    if (impl_->stopped || impl_->offered || impl_->channel || impl_->peer->remoteDescription()) {
        throw std::logic_error("Offer cannot start on this transport state");
    }
    impl_->offered = true;
    impl_->attach(impl_->peer->createDataChannel(reliable_channel_label));
    impl_->peer->setLocalDescription(rtc::Description::Type::Offer);
}

void NativeTransport::set_remote_description(rtc::Description description) {
    if (impl_->stopped) {
        throw std::logic_error("Transport is closed");
    }
    if (description.type() != rtc::Description::Type::Offer &&
        description.type() != rtc::Description::Type::Answer) {
        throw std::invalid_argument("Only offer/answer descriptions are supported");
    }
    const bool offer = description.type() == rtc::Description::Type::Offer;
    if (impl_->remote_description_set || offer == impl_->offered) {
        throw std::logic_error("Unexpected description or unsupported renegotiation");
    }
    impl_->peer->setRemoteDescription(std::move(description));
    impl_->remote_description_set = true;
    if (offer) {
        impl_->peer->setLocalDescription(rtc::Description::Type::Answer);
    }
}

void NativeTransport::add_remote_candidate(rtc::Candidate candidate) {
    if (impl_->stopped) {
        throw std::logic_error("Transport is closed");
    }
    impl_->peer->addRemoteCandidate(std::move(candidate));
}

NativeSendResult NativeTransport::send(ByteView bytes) {
    if (impl_->stopped) {
        return NativeSendResult::NotReady;
    }
    impl_->update_readiness(false);
    if (impl_->status != NativeTransportState::Ready) {
        return NativeSendResult::NotReady;
    }
    if (bytes.size() > impl_->channel->maxMessageSize()) {
        return NativeSendResult::TooLarge;
    }
    try {
        // With the required patched backend, false means accepted but buffered.
        impl_->channel->send(rtc::binary(bytes.begin(), bytes.end()));
        return NativeSendResult::Accepted;
    } catch (const std::exception &error) {
        impl_->fail(std::string("Binary send failed in DataChannel backend: ") + error.what());
        return NativeSendResult::Failed;
    }
}

void NativeTransport::set_writable_threshold(std::size_t bytes) {
    impl_->writable_threshold = bytes;
    if (impl_->channel)
        impl_->channel->setBufferedAmountLowThreshold(bytes);
}

void NativeTransport::enable_datagrams() {
    auto &impl = *impl_;
    if (impl.stopped || impl.datagrams || !impl.callbacks.on_datagram)
        return;
    impl.datagrams = std::make_unique<NativeDatagramChannel>(
        impl.executor, [weak = impl.weak_from_this()](Bytes value) {
            auto self = weak.lock();
            if (self && self->active && !self->stopped &&
                self->status == NativeTransportState::Ready)
                self->callbacks.on_datagram(std::move(value));
        });
    if (impl.offered)
        impl.datagrams->open(*impl.peer);
}

bool NativeTransport::datagrams_available() const {
    return !impl_->stopped && impl_->status == NativeTransportState::Ready && impl_->datagrams &&
           impl_->datagrams->is_open();
}

ChannelSend NativeTransport::send_datagram(ByteView bytes) {
    if (impl_->stopped)
        return ChannelSend::Closed;
    if (impl_->status != NativeTransportState::Ready || !impl_->datagrams)
        return bytes.size() > max_datagram_bytes ? ChannelSend::TooLarge : ChannelSend::NotReady;
    return impl_->datagrams->send(bytes);
}

std::size_t NativeTransport::writable_threshold() const { return impl_->writable_threshold; }

std::size_t NativeTransport::buffered_amount() const {
    return impl_->channel ? impl_->channel->bufferedAmount() : 0;
}
std::size_t NativeTransport::max_message_size() const {
    return impl_->channel ? impl_->channel->maxMessageSize() : 0;
}
NativeTransportState NativeTransport::state() const { return impl_->status; }
RouteInfo NativeTransport::poll_route() {
    if (!impl_->stopped) {
        impl_->update_readiness();
    }
    return impl_->tracker.route();
}
void NativeTransport::resume_receive() { impl_->resume_receive(); }
void NativeTransport::close() {
    {
        std::lock_guard lock(impl_->submission_gate->mutex);
        if (!impl_->active.exchange(false)) {
            return;
        }
        impl_->submission_gate->accepts = false;
    }
    impl_->status = NativeTransportState::Closed;
    impl_->shutdown();
}
} // namespace p2pdc::detail
