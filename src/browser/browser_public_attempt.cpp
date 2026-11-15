#include "../transport/public_attempt.hpp"
#include "browser_public_config.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
struct State {
    ConnectionAttemptCallbacks callbacks;
    std::unique_ptr<BrowserMailboxAttempt> attempt;
    ConnectionAttemptState current = ConnectionAttemptState::Waiting;
    bool authenticated = false;
};

ConnectError public_failure(AttemptFailure reason) {
    switch (reason) {
    case AttemptFailure::Signaling:
        return ConnectError::Signaling;
    case AttemptFailure::Protocol:
        return ConnectError::Protocol;
    case AttemptFailure::Transport:
        return ConnectError::Transport;
    case AttemptFailure::Resource:
        return ConnectError::ResourceLimit;
    case AttemptFailure::Timeout:
        return ConnectError::Timeout;
    case AttemptFailure::Rejected:
        return ConnectError::Rejected;
    case AttemptFailure::Canceled:
        return ConnectError::Canceled;
    }
    return ConnectError::Backend;
}

BrowserMailboxCallbacks handlers(const std::shared_ptr<State> &state) {
    std::weak_ptr<State> weak = state;
    return {[weak](std::uint32_t) {
                if (auto s = weak.lock()) {
                    s->authenticated = true;
                    s->current = ConnectionAttemptState::Ready;
                    s->callbacks.on_ready();
                }
            },
            [weak](Bytes value) {
                if (auto s = weak.lock())
                    s->callbacks.on_message(std::move(value));
            },
            [weak] {
                if (auto s = weak.lock())
                    s->callbacks.on_writable();
            },
            [weak](RouteInfo value) {
                if (auto s = weak.lock())
                    s->callbacks.on_route(std::move(value));
            },
            [weak](BrowserTransportState value) {
                if (auto s = weak.lock(); s && s->authenticated) {
                    if (value == BrowserTransportState::Suspended)
                        s->current = ConnectionAttemptState::Suspended;
                    else if (value == BrowserTransportState::Ready)
                        s->current = ConnectionAttemptState::Ready;
                }
            },
            [] {},
            [weak](AttemptFailure) {
                if (auto s = weak.lock()) {
                    s->current = ConnectionAttemptState::Failed;
                    s->callbacks.on_failed();
                }
            }};
}
class BrowserPublicAttempt final : public PublicAttempt {
  public:
    explicit BrowserPublicAttempt(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ConnectionAttemptStart poll_start() override;
    ChannelSend send(ByteView value) override { return state_->attempt->send(value); }
    ConnectionAttemptState state() const override { return state_->current; }
    ConnectError failure_reason() const override {
        return public_failure(state_->attempt->failure_reason());
    }
    std::optional<std::uint32_t> max_message_size() const override {
        return state_->attempt->max_message_size();
    }
    bool signaling_unavailable() const override { return state_->attempt->signaling_unavailable(); }
    void poll_health() override { state_->attempt->poll_health(); }
    RouteInfo poll_route() override {
        state_->attempt->poll_route();
        return state_->attempt->route();
    }
    void close() override {
        state_->current = ConnectionAttemptState::Closed;
        state_->attempt->close();
    }

  private:
    std::shared_ptr<State> state_;
};
ConnectionAttemptStart BrowserPublicAttempt::poll_start() {
    switch (state_->attempt->poll_start()) {
    case BrowserMailboxStart::Pending:
        return ConnectionAttemptStart::Pending;
    case BrowserMailboxStart::Started:
        state_->current = ConnectionAttemptState::Connecting;
        return ConnectionAttemptStart::Started;
    case BrowserMailboxStart::AlreadyStarted:
        return ConnectionAttemptStart::AlreadyStarted;
    case BrowserMailboxStart::Failed:
        return ConnectionAttemptStart::Failed;
    case BrowserMailboxStart::Closed:
        return ConnectionAttemptStart::Closed;
    }
    return ConnectionAttemptStart::Failed;
}
std::uint64_t now_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("system clock is before the Unix epoch");
    return static_cast<std::uint64_t>(value);
}
} // namespace

std::variant<std::unique_ptr<PublicAttempt>, StoreError>
make_public_attempt(const ConnectionAdmission &admission, const IncomingOffer *offer, IceConfig ice,
                    MemoryBudget budget, std::uint32_t message_limit, Executor executor,
                    ConnectionAttemptCallbacks callbacks) {
    auto config = make_browser_public_config(admission, offer, std::move(ice), std::move(budget),
                                             message_limit, now_seconds);
    auto state = std::make_shared<State>();
    state->callbacks = std::move(callbacks);
    auto created =
        BrowserMailboxAttempt::create(std::move(config), std::move(executor), handlers(state));
    if (auto *error = std::get_if<StoreError>(&created))
        return *error;
    state->attempt = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(created));
    return std::unique_ptr<PublicAttempt>(new BrowserPublicAttempt(std::move(state)));
}
} // namespace p2pdc::detail
