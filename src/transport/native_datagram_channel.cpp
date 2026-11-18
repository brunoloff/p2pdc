#include "native_datagram_channel.hpp"

#include "datagram_support.hpp"
#include "p2pdc/connection.h"

#include <atomic>
#include <string>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::size_t max_pending_deliveries = 32;
// Beyond this much unsent data, new datagrams would only arrive late.
constexpr std::size_t max_buffered_bytes = 64U * 1024U;
} // namespace

struct NativeDatagramChannel::Receiver : std::enable_shared_from_this<Receiver> {
    Receiver(Executor dispatch, std::function<void(Bytes)> handler)
        : executor(std::move(dispatch)), deliver(std::move(handler)) {}

    // Runs on a backend thread.
    void receive(rtc::message_variant message) {
        auto *binary = std::get_if<rtc::binary>(&message);
        if (!binary || binary->size() > max_datagram_bytes || !active)
            return;
        if (pending.fetch_add(1) >= max_pending_deliveries) {
            pending.fetch_sub(1);
            return;
        }
        executor([self = shared_from_this(), bytes = std::move(*binary)]() mutable {
            self->pending.fetch_sub(1);
            if (self->active)
                self->deliver(std::move(bytes));
        });
    }

    Executor executor;
    std::function<void(Bytes)> deliver;
    std::atomic<bool> active{true};
    std::atomic<std::size_t> pending{0};
};

NativeDatagramChannel::NativeDatagramChannel(Executor executor, std::function<void(Bytes)> deliver)
    : receiver_(std::make_shared<Receiver>(std::move(executor), std::move(deliver))) {}

NativeDatagramChannel::~NativeDatagramChannel() { close(); }

void NativeDatagramChannel::attach(std::shared_ptr<rtc::DataChannel> channel) {
    channel_ = std::move(channel);
    channel_->onMessage([receiver = receiver_](rtc::message_variant message) {
        receiver->receive(std::move(message));
    });
}

void NativeDatagramChannel::open(rtc::PeerConnection &peer) {
    rtc::DataChannelInit init;
    init.reliability.unordered = true;
    init.reliability.maxRetransmits = 0;
    attach(peer.createDataChannel(std::string(datagram_channel_label), std::move(init)));
}

bool NativeDatagramChannel::adopt(std::shared_ptr<rtc::DataChannel> channel) {
    const auto reliability = channel->reliability();
    if (channel_ || !reliability.unordered || reliability.maxPacketLifeTime ||
        reliability.maxRetransmits != 0U) {
        return false;
    }
    attach(std::move(channel));
    return true;
}

bool NativeDatagramChannel::is_open() const { return channel_ && channel_->isOpen(); }

ChannelSend NativeDatagramChannel::send(ByteView bytes) {
    if (bytes.size() > max_datagram_bytes)
        return ChannelSend::TooLarge;
    if (!is_open())
        return ChannelSend::NotReady;
    if (channel_->bufferedAmount() > max_buffered_bytes)
        return ChannelSend::Backpressure;
    try {
        channel_->send(rtc::binary(bytes.begin(), bytes.end()));
        return ChannelSend::Accepted;
    } catch (const std::exception &) {
        return ChannelSend::Failed; // a datagram may be lost; the connection stays up
    }
}

void NativeDatagramChannel::close() {
    receiver_->active = false;
    if (channel_) {
        channel_->resetCallbacks();
        channel_->close();
        channel_.reset();
    }
}

} // namespace p2pdc::detail
