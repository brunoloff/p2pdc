#pragma once

#include "../discovery/record_store.hpp"
#include "../protocol/reliable_channel.hpp"
#include "p2pdc/connection.h"

namespace p2pdc::detail {
struct ConnectionAdmission;
struct IncomingOffer;
class NativeAttempt;

enum class ConnectionAttemptState {
    Waiting,
    Connecting,
    Authenticating,
    Ready,
    Suspended,
    Failed,
    Closed
};
enum class ConnectionAttemptStart { Pending, Started, AlreadyStarted, Failed, Closed };
struct ConnectionAttemptCallbacks {
    std::function<void()> on_ready;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(RouteInfo)> on_route;
    std::function<void()> on_failed;
    std::function<void(Bytes)> on_datagram = {}; // only after on_ready; may be dropped
};

class PublicAttempt {
  public:
    virtual ~PublicAttempt() = default;
    [[nodiscard]] virtual ConnectionAttemptStart poll_start() = 0;
    [[nodiscard]] virtual ChannelSend send(ByteView payload) = 0;
    [[nodiscard]] virtual ChannelSend send_datagram(ByteView payload) = 0;
    [[nodiscard]] virtual bool datagrams_available() const = 0;
    [[nodiscard]] virtual ConnectionAttemptState state() const = 0;
    [[nodiscard]] virtual ConnectError failure_reason() const = 0;
    [[nodiscard]] virtual std::optional<std::uint32_t> max_message_size() const = 0;
    [[nodiscard]] virtual bool signaling_unavailable() const = 0;
    virtual void poll_health() = 0;
    [[nodiscard]] virtual RouteInfo poll_route() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual NativeAttempt *native_for_testing() { return nullptr; }
};

[[nodiscard]] std::variant<std::unique_ptr<PublicAttempt>, StoreError>
make_public_attempt(const ConnectionAdmission &admission, const IncomingOffer *offer, IceConfig ice,
                    MemoryBudget budget, std::uint32_t message_limit, bool datagrams,
                    Executor executor, ConnectionAttemptCallbacks callbacks);
} // namespace p2pdc::detail
