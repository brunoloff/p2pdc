#pragma once

#include "../security/secure_attempt.hpp"
#include "browser_transport.hpp"

namespace p2pdc::detail {
struct BrowserSecureAttemptImpl;
struct BrowserSecureConfig {
    AsyncSignalingBinding binding;
    std::shared_ptr<const Identity> identity;
    std::shared_ptr<const RecipientKeyPair> recipient;
    std::shared_ptr<const Bytes> capability;
    IceConfig ice;
    std::uint32_t max_message_bytes = 256 * 1024;
    std::function<std::uint64_t()> now;
    std::uint64_t clock_skew = 30;
};
struct BrowserSecureCallbacks {
    // Must enqueue and return without reentering or destroying this attempt.
    std::function<bool(Bytes)> submit_signal;
    std::function<void(std::uint32_t)> on_ready;
    std::function<void(Bytes)> on_message;
    std::function<void()> on_writable;
    std::function<void(RouteInfo)> on_route;
    std::function<void(BrowserTransportState)> on_transport;
    std::function<void(AttemptFailure)> on_failure;
};
// Internal preauthorized attempt. The owner supplies verified peer bindings,
// a nondecreasing UTC clock, and ordered executor. All callbacks are serialized,
// nonthrowing and non-reentrant. Signaling is authenticated but best effort;
// the owner must enforce a deadline and call timeout(). Close suppresses later
// callbacks. This is not a public discovery or consent interface.
class BrowserSecureAttempt {
  public:
    BrowserSecureAttempt(BrowserSecureConfig config, MemoryBudget budget, Executor executor,
                         BrowserSecureCallbacks callbacks);
    ~BrowserSecureAttempt();
    BrowserSecureAttempt(const BrowserSecureAttempt &) = delete;
    BrowserSecureAttempt &operator=(const BrowserSecureAttempt &) = delete;
    [[nodiscard]] bool start();
    [[nodiscard]] FlowResult receive(ByteView signal);
    [[nodiscard]] ChannelSend send(ByteView message);
    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::optional<std::uint32_t> max_message_size() const;
    [[nodiscard]] RouteInfo route() const;
    void poll_route();
    void timeout();
    void close();

  private:
    std::shared_ptr<BrowserSecureAttemptImpl> impl_;
};
} // namespace p2pdc::detail
