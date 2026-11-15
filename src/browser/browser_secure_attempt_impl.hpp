#pragma once

#include "browser_secure_attempt.hpp"

namespace p2pdc::detail {
struct BrowserSecureAttemptImpl : ReliableSink,
                                  std::enable_shared_from_this<BrowserSecureAttemptImpl> {
    BrowserSecureAttemptImpl(BrowserSecureConfig config, Executor executor,
                             BrowserSecureCallbacks callbacks);
    void initialize(MemoryBudget budget);
    [[nodiscard]] BrowserTransportCallbacks transport_callbacks();
    [[nodiscard]] SecureAttemptCallbacks secure_callbacks();
    [[nodiscard]] SignalingFlowCallbacks signaling_callbacks();
    [[nodiscard]] Executor guarded_executor();
    [[nodiscard]] std::uint64_t time();
    [[nodiscard]] SignalingPolicy policy(std::uint64_t now) const;
    void description(std::string value);
    void candidate(std::string value, std::string mid);
    void state(BrowserTransportState value);
    void route_changed(RouteInfo value);
    void fail(AttemptFailure reason);
    void close();
    [[nodiscard]] std::size_t buffered_amount() const override;
    [[nodiscard]] SinkResult send(ByteView frame) override;
    BrowserSecureConfig config;
    Executor executor;
    BrowserSecureCallbacks callbacks;
    std::unique_ptr<BrowserTransport> transport;
    std::unique_ptr<SecureAttempt> attempt;
    std::optional<std::uint64_t> last_time;
    std::optional<std::uint32_t> negotiated;
    RouteInfo observed_route;
    bool active = true, failed = false, started = false;
};
} // namespace p2pdc::detail
