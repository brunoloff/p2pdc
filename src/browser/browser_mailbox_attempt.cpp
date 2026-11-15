#include "../discovery/signal_errors.hpp"
#include "../discovery/terminal_signal.hpp"
#include "../security/signaling_limits.hpp"
#include "browser_mailbox_attempt_impl.hpp"

#include <emscripten.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
void validate(const BrowserMailboxConfig &config, const Executor &executor,
              const BrowserMailboxCallbacks &callbacks) {
    if (!executor || !callbacks.on_ready || !callbacks.on_message || !callbacks.on_writable ||
        !callbacks.on_route || !callbacks.on_transport || !callbacks.on_signaling_unavailable ||
        !callbacks.on_failure || config.budget.limit() < 524288 + signaling_wire_limit ||
        (config.accepted_offer &&
         (config.secure.binding.role != HandshakeRole::Answerer || config.accepted_offer->empty() ||
          config.accepted_offer->size() > signaling_wire_limit)))
        throw std::invalid_argument("invalid browser mailbox attempt settings");
}
void defer_cleanup(std::unique_ptr<std::weak_ptr<BrowserMailboxAttemptImpl>> holder) noexcept {
    emscripten_async_call(
        [](void *value) {
            std::unique_ptr<std::weak_ptr<BrowserMailboxAttemptImpl>> owner(
                static_cast<std::weak_ptr<BrowserMailboxAttemptImpl> *>(value));
            if (auto state = owner->lock(); state && state->active && state->failed)
                state->stop();
        },
        holder.release(), 0);
}
} // namespace
BrowserMailboxAttemptImpl::BrowserMailboxAttemptImpl(BrowserMailboxConfig value, Executor dispatch,
                                                     BrowserMailboxCallbacks handlers)
    : config(std::move(value)), executor(std::move(dispatch)), callbacks(std::move(handlers)) {}
std::optional<StoreError> BrowserMailboxAttemptImpl::initialize() {
    emergency_cleanup =
        std::make_unique<std::weak_ptr<BrowserMailboxAttemptImpl>>(weak_from_this());
    const SignalPipeLimits limits{{524288, 128, signaling_wire_limit},
                                  {524288, 64, signaling_wire_limit}};
    auto created =
        NativeSignalPipe::create(config.store, config.local_mailbox, config.remote_mailbox,
                                 config.budget, limits, executor, pipe_callbacks());
    if (auto error = std::get_if<StoreError>(&created))
        return *error;
    pipe = std::move(std::get<std::unique_ptr<NativeSignalPipe>>(created));
    secure = std::make_unique<BrowserSecureAttempt>(config.secure, config.budget, executor,
                                                    secure_callbacks());
    return std::nullopt;
}
bool BrowserMailboxAttemptImpl::apply_offer() {
    if (!config.accepted_offer)
        return true;
    auto offer = std::move(*config.accepted_offer);
    config.accepted_offer.reset();
    const auto now = config.secure.now();
    const auto decoded = decode_signaling(
        offer, *config.secure.recipient, config.secure.binding.local, *config.secure.capability,
        {now, 60, config.secure.clock_skew, signaling_payload_limit});
    const auto envelope = std::get_if<SignalingEnvelope>(&decoded);
    return envelope && envelope->fields.kind == SignalingKind::Offer &&
           secure->receive(offer) == FlowResult::Accepted;
}
void BrowserMailboxAttemptImpl::fail(AttemptFailure reason) {
    if (!active || failed)
        return;
    failed = true;
    this->reason = reason;
    auto weak = weak_from_this();
    try {
        executor([weak, reason] {
            if (auto self = weak.lock())
                self->finish_failure(reason);
        });
    } catch (...) {
        std::cerr << "p2pdc: mailbox attempt failure delivery failed; close the attempt\n";
        defer_cleanup(std::move(emergency_cleanup));
    }
}
void BrowserMailboxAttemptImpl::finish_failure(AttemptFailure reason) {
    if (!active || !failed)
        return;
    stop();
    callbacks.on_failure(reason);
}
void BrowserMailboxAttemptImpl::stop() {
    if (pipe)
        pipe->close();
    if (secure)
        secure->close();
}
void BrowserMailboxAttemptImpl::cancel_remote() noexcept {
    try {
        const auto now = config.secure.now();
        if (now > std::numeric_limits<std::uint64_t>::max() - 50)
            throw std::overflow_error("cancellation expiry overflow");
        const auto &binding = config.secure.binding;
        SignalingFields fields{SignalingKind::Cancel,
                               binding.local.instance,
                               binding.remote,
                               binding.attempt,
                               cancel_sequence,
                               now,
                               now + 50,
                               {}};
        (void)publish_terminal_signal(config.store, config.remote_mailbox, *config.secure.identity,
                                      std::move(fields), binding.remote_box,
                                      *config.secure.capability,
                                      {now, 60, config.secure.clock_skew, signaling_payload_limit});
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: could not publish cancellation: " << error.what() << '\n';
    }
}
void BrowserMailboxAttemptImpl::close() {
    if (!active)
        return;
    if ((started || config.accepted_offer) && !authenticated && !failed)
        cancel_remote();
    active = false;
    stop();
    config.accepted_offer.reset();
}
BrowserMailboxAttempt::BrowserMailboxAttempt(std::shared_ptr<BrowserMailboxAttemptImpl> impl)
    : impl_(std::move(impl)) {}
BrowserMailboxAttempt::~BrowserMailboxAttempt() { close(); }
std::variant<std::unique_ptr<BrowserMailboxAttempt>, StoreError>
BrowserMailboxAttempt::create(BrowserMailboxConfig config, Executor executor,
                              BrowserMailboxCallbacks callbacks) {
    validate(config, executor, callbacks);
    auto state = std::make_shared<BrowserMailboxAttemptImpl>(std::move(config), std::move(executor),
                                                             std::move(callbacks));
    if (auto error = state->initialize())
        return *error;
    return std::unique_ptr<BrowserMailboxAttempt>(new BrowserMailboxAttempt(std::move(state)));
}
BrowserMailboxStart BrowserMailboxAttempt::poll_start() {
    auto state = impl_;
    if (!state->active)
        return BrowserMailboxStart::Closed;
    if (state->failed)
        return BrowserMailboxStart::Failed;
    if (state->started)
        return BrowserMailboxStart::AlreadyStarted;
    if (!state->pipe->is_open()) {
        state->fail(AttemptFailure::Signaling);
        return BrowserMailboxStart::Failed;
    }
    auto registration = state->pipe->registration();
    if (registration.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return BrowserMailboxStart::Pending;
    try {
        if (!registration.get() || !state->secure->start())
            throw std::runtime_error("startup failed");
        state->started = true;
        if (!state->apply_offer())
            throw std::runtime_error("accepted offer rejected");
        auto catchup = state->pipe->catch_up();
        if (!std::holds_alternative<std::monostate>(catchup)) {
            state->fail(resource_catchup_error(catchup) ? AttemptFailure::Resource
                                                        : AttemptFailure::Signaling);
            return BrowserMailboxStart::Failed;
        }
        return BrowserMailboxStart::Started;
    } catch (const std::exception &) {
        state->fail(AttemptFailure::Signaling);
        return BrowserMailboxStart::Failed;
    }
}
ChannelSend BrowserMailboxAttempt::send(ByteView value) {
    auto state = impl_;
    if (!state->active)
        return ChannelSend::Closed;
    if (state->failed)
        return ChannelSend::Failed;
    return state->secure->send(value);
}
bool BrowserMailboxAttempt::ready() const {
    return impl_->active && !impl_->failed && impl_->secure->ready();
}
bool BrowserMailboxAttempt::signaling_unavailable() const { return impl_->unavailable; }
AttemptFailure BrowserMailboxAttempt::failure_reason() const { return impl_->reason; }
std::optional<std::uint32_t> BrowserMailboxAttempt::max_message_size() const {
    return impl_->active && !impl_->failed ? impl_->secure->max_message_size() : std::nullopt;
}
RouteInfo BrowserMailboxAttempt::route() const { return impl_->secure->route(); }
void BrowserMailboxAttempt::poll_health() {
    auto state = impl_;
    if (state->active && !state->failed && state->started && !state->unavailable &&
        !state->pipe->is_open())
        state->fail(AttemptFailure::Signaling);
    else if (state->active && !state->failed && state->started && !state->unavailable)
        state->pipe->retry_budget();
}
void BrowserMailboxAttempt::poll_route() {
    if (impl_->active && !impl_->failed)
        impl_->secure->poll_route();
}
void BrowserMailboxAttempt::timeout() {
    auto state = impl_;
    state->fail(AttemptFailure::Timeout);
}
void BrowserMailboxAttempt::close() {
    if (impl_)
        impl_->close();
}
} // namespace p2pdc::detail
