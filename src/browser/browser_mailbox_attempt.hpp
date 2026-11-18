#pragma once

#include "../discovery/native_signal_pipe.hpp"
#include "browser_secure_attempt.hpp"

namespace p2pdc::detail {
struct BrowserMailboxAttemptImpl;
enum class BrowserMailboxStart { Pending, Started, AlreadyStarted, Failed, Closed };
struct BrowserMailboxConfig {
    RecordStore store;
    std::array<std::byte, 20> local_mailbox;
    std::array<std::byte, 20> remote_mailbox;
    BrowserSecureConfig secure;
    MemoryBudget budget;
    std::optional<Bytes> accepted_offer;
};
struct BrowserMailboxCallbacks {
    std::function<void(std::uint32_t)> on_ready;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(RouteInfo)> on_route;
    std::function<void(BrowserTransportState)> on_transport;
    std::function<void()> on_signaling_unavailable;
    std::function<void(AttemptFailure)> on_failure;
    std::function<void(Bytes)> on_datagram = {};
};
// Internal preauthorized single attempt. The caller owns discovery, consent,
// deadlines and polling. Methods and callbacks run on one serialized executor;
// callbacks must not throw, reenter or destroy this object. The backend and
// executor outlive outstanding work. DHT signaling can be lost or duplicated.
class BrowserMailboxAttempt {
  public:
    [[nodiscard]] static std::variant<std::unique_ptr<BrowserMailboxAttempt>, StoreError>
    create(BrowserMailboxConfig config, Executor executor, BrowserMailboxCallbacks callbacks);
    ~BrowserMailboxAttempt();
    BrowserMailboxAttempt(const BrowserMailboxAttempt &) = delete;
    BrowserMailboxAttempt &operator=(const BrowserMailboxAttempt &) = delete;
    [[nodiscard]] BrowserMailboxStart poll_start();
    [[nodiscard]] ChannelSend send(ByteView value);
    [[nodiscard]] ChannelSend send_datagram(ByteView value);
    [[nodiscard]] bool datagrams_available() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool signaling_unavailable() const;
    [[nodiscard]] AttemptFailure failure_reason() const;
    [[nodiscard]] std::optional<std::uint32_t> max_message_size() const;
    [[nodiscard]] RouteInfo route() const;
    void poll_health();
    void poll_route();
    void timeout();
    void close();

  private:
    explicit BrowserMailboxAttempt(std::shared_ptr<BrowserMailboxAttemptImpl> impl);
    std::shared_ptr<BrowserMailboxAttemptImpl> impl_;
};
} // namespace p2pdc::detail
