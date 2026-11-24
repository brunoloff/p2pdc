#include "../security/signaling_limits.hpp"
#include "browser_secure_attempt_impl.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
void validate(const BrowserSecureConfig &config, const Executor &executor,
              const BrowserSecureCallbacks &callbacks) {
    if (!executor || !config.identity || !config.recipient || !config.capability ||
        config.capability->size() != capability_size || !config.now || !config.max_message_bytes ||
        config.max_message_bytes > 4 * 1024 * 1024 || !callbacks.submit_signal ||
        !callbacks.on_ready || !callbacks.on_message || !callbacks.on_writable ||
        !callbacks.on_route || !callbacks.on_transport || !callbacks.on_failure)
        throw std::invalid_argument("invalid browser secure attempt settings");
}
} // namespace
BrowserSecureAttemptImpl::BrowserSecureAttemptImpl(BrowserSecureConfig value, Executor dispatch,
                                                   BrowserSecureCallbacks handlers)
    : config(std::move(value)), executor(std::move(dispatch)), callbacks(std::move(handlers)) {}
void BrowserSecureAttemptImpl::initialize(MemoryBudget budget) {
    const auto role = config.binding.role == HandshakeRole::Offerer ? BrowserRole::Offerer
                                                                    : BrowserRole::Answerer;
    transport = std::make_unique<BrowserTransport>(role, config.ice, BrowserTransportLimits{},
                                                   transport_callbacks());
    const auto limit = std::max<std::uint32_t>(262145, config.max_message_bytes + 1);
    const ReliableChannelLimits limits{
        {limit, 1024}, std::max<std::size_t>(2097152, limit), 32, 524288, 4};
    attempt = std::make_unique<SecureAttempt>(config.binding, *config.capability, config.identity,
                                              *config.recipient, CandidateLimits{4096, 256, 4361},
                                              std::move(budget), limits, config.max_message_bytes,
                                              *this, guarded_executor(), secure_callbacks());
}
std::uint64_t BrowserSecureAttemptImpl::time() {
    const auto now = config.now();
    if ((last_time && now < *last_time) || now > std::numeric_limits<std::uint64_t>::max() - 50)
        throw std::runtime_error("invalid or regressing signaling clock");
    last_time = now;
    return now;
}
SignalingPolicy BrowserSecureAttemptImpl::policy(std::uint64_t now) const {
    return {now, 60, config.clock_skew, signaling_payload_limit};
}
void BrowserSecureAttemptImpl::fail(AttemptFailure reason) {
    if (!active || failed)
        return;
    failed = true;
    negotiated.reset();
    observed_route.current = RouteKind::Unknown;
    observed_route.selected_pair.reset();
    observed_route.observed_at.reset();
    observed_route.round_trip_time.reset();
    if (transport)
        transport->close();
    if (attempt)
        attempt->close();
    callbacks.on_failure(reason);
}
void BrowserSecureAttemptImpl::close() {
    if (!active)
        return;
    active = false;
    negotiated.reset();
    observed_route.current = RouteKind::Unknown;
    observed_route.selected_pair.reset();
    observed_route.observed_at.reset();
    observed_route.round_trip_time.reset();
    if (transport)
        transport->close();
    if (attempt)
        attempt->close();
}
std::size_t BrowserSecureAttemptImpl::buffered_amount() const {
    return active && !failed && transport
               ? transport->buffered_amount().value_or(std::numeric_limits<std::size_t>::max())
               : std::numeric_limits<std::size_t>::max();
}
SinkResult BrowserSecureAttemptImpl::send(ByteView frame) {
    if (!active || failed || !transport)
        return SinkResult::Failed;
    switch (transport->send(frame)) {
    case BrowserSend::Accepted:
        return SinkResult::Accepted;
    case BrowserSend::NotReady:
    case BrowserSend::Backpressure:
        return SinkResult::NotReady;
    default:
        return SinkResult::Failed;
    }
}
BrowserSecureAttempt::BrowserSecureAttempt(BrowserSecureConfig config, MemoryBudget budget,
                                           Executor executor, BrowserSecureCallbacks callbacks) {
    validate(config, executor, callbacks);
    auto state = std::make_shared<BrowserSecureAttemptImpl>(std::move(config), std::move(executor),
                                                            std::move(callbacks));
    state->initialize(std::move(budget));
    impl_ = std::move(state);
}
BrowserSecureAttempt::~BrowserSecureAttempt() { close(); }
bool BrowserSecureAttempt::start() {
    auto state = impl_;
    if (!state->active || state->failed || state->started)
        return false;
    state->started = true;
    if (state->config.binding.role == HandshakeRole::Offerer && !state->transport->start_offer()) {
        state->fail(AttemptFailure::Transport);
        return false;
    }
    return true;
}
FlowResult BrowserSecureAttempt::receive(ByteView signal) {
    auto state = impl_;
    if (!state->active)
        return FlowResult::Closed;
    if (!state->started || state->failed)
        return FlowResult::Failed;
    try {
        return state->attempt->receive_signaling(signal, state->policy(state->time()));
    } catch (const std::exception &) {
        state->fail(AttemptFailure::Signaling);
        return FlowResult::Failed;
    }
}
ChannelSend BrowserSecureAttempt::send(ByteView message) {
    auto state = impl_;
    if (!state->active)
        return ChannelSend::Closed;
    if (state->failed)
        return ChannelSend::Failed;
    return state->attempt->send(message);
}
// Datagrams count only once the handshake has authenticated the peer behind the
// connection's DTLS session.
ChannelSend BrowserSecureAttempt::send_datagram(ByteView message) {
    auto state = impl_;
    if (!state->active)
        return ChannelSend::Closed;
    if (state->failed)
        return ChannelSend::Failed;
    if (!state->attempt->ready())
        return ChannelSend::NotReady;
    switch (state->transport->send_datagram(message)) {
    case BrowserSend::Accepted:
        return ChannelSend::Accepted;
    case BrowserSend::NotReady:
        return ChannelSend::NotReady;
    case BrowserSend::TooLarge:
        return ChannelSend::TooLarge;
    case BrowserSend::Backpressure:
        return ChannelSend::Backpressure;
    case BrowserSend::Closed:
        return ChannelSend::Closed;
    case BrowserSend::Failed:
        return ChannelSend::Failed;
    }
    return ChannelSend::Failed;
}
bool BrowserSecureAttempt::datagrams_available() const {
    return ready() && impl_->transport->datagrams_available();
}
bool BrowserSecureAttempt::ready() const {
    return impl_->active && !impl_->failed && impl_->attempt->ready();
}
std::optional<std::uint32_t> BrowserSecureAttempt::max_message_size() const {
    return impl_->negotiated;
}
RouteInfo BrowserSecureAttempt::route() const { return impl_->observed_route; }
void BrowserSecureAttempt::poll_route() {
    if (impl_->active && !impl_->failed)
        impl_->transport->poll_route();
}
void BrowserSecureAttempt::timeout() {
    auto state = impl_;
    if (state->active && !state->failed)
        state->fail(AttemptFailure::Timeout);
}
void BrowserSecureAttempt::close() {
    if (impl_)
        impl_->close();
}
} // namespace p2pdc::detail
