#pragma once

#include "../protocol/reliable_channel.hpp"
#include "p2pdc/frame.h"
#include "p2pdc/operation.h"

#include <rtc/peerconnection.hpp>

#include <functional>
#include <memory>

namespace p2pdc::detail {

// The unreliable data channel of one connection. Messages may be lost or reordered;
// anything that does not fit the limits is dropped rather than failing the connection.
// Methods run on the transport's serialized context. Received messages are posted to
// `executor`, at most a small bounded number at a time, and `deliver` runs there.
class NativeDatagramChannel {
  public:
    NativeDatagramChannel(Executor executor, std::function<void(Bytes)> deliver);
    ~NativeDatagramChannel();
    NativeDatagramChannel(const NativeDatagramChannel &) = delete;
    NativeDatagramChannel &operator=(const NativeDatagramChannel &) = delete;
    // Offerer: creates the channel.
    void open(rtc::PeerConnection &peer);
    // Answerer: takes the peer's channel; false when it is not unordered and
    // retransmission-free or one is already attached.
    [[nodiscard]] bool adopt(std::shared_ptr<rtc::DataChannel> channel);
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] ChannelSend send(ByteView bytes);
    void close();

  private:
    struct Receiver;
    void attach(std::shared_ptr<rtc::DataChannel> channel);
    std::shared_ptr<Receiver> receiver_;
    std::shared_ptr<rtc::DataChannel> channel_;
};

} // namespace p2pdc::detail
