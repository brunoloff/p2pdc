#include "../transport/datagram_support.hpp"
#include "browser_secure_attempt_impl.hpp"

#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
ByteView bytes(const std::string &value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}
} // namespace
Executor BrowserSecureAttemptImpl::guarded_executor() {
    auto weak = weak_from_this();
    auto dispatch = executor;
    return [weak, dispatch](std::function<void()> task) {
        dispatch([weak, task = std::move(task)]() mutable {
            if (auto self = weak.lock(); self && self->active && !self->failed)
                task();
        });
    };
}
void BrowserSecureAttemptImpl::description(std::string value) {
    if (!active || failed || !started)
        return;
    try {
        const auto now = time();
        if (config.datagrams)
            value =
                advertise_datagrams(std::move(value), config.binding.role == HandshakeRole::Offerer
                                                          ? datagram_offer_attribute
                                                          : datagram_answer_attribute);
        if (attempt->local_description(bytes(value), now, now + 50, policy(now)) !=
            FlowResult::Accepted)
            fail(AttemptFailure::Signaling);
    } catch (const std::exception &) {
        fail(AttemptFailure::Signaling);
    }
}
void BrowserSecureAttemptImpl::candidate(std::string value, std::string mid) {
    if (!active || failed || !started)
        return;
    try {
        const auto now = time();
        if (attempt->local_candidate({value, mid}, now, now + 50, policy(now)) !=
            FlowResult::Accepted)
            fail(AttemptFailure::Signaling);
    } catch (const std::exception &) {
        fail(AttemptFailure::Signaling);
    }
}
void BrowserSecureAttemptImpl::state(BrowserTransportState value) {
    if (!active || failed)
        return;
    if (value == BrowserTransportState::Failed || value == BrowserTransportState::Closed) {
        fail(AttemptFailure::Transport);
        return;
    }
    if (value == BrowserTransportState::Ready) {
        const auto maximum = transport->max_message_size();
        if (!maximum || (*maximum && *maximum < 1024)) {
            fail(AttemptFailure::Transport);
            return;
        }
        attempt->set_transport_ready(true);
        attempt->wake();
    } else if (value == BrowserTransportState::Suspended) {
        attempt->set_transport_ready(false);
    }
    if (active && !failed)
        callbacks.on_transport(value);
}
void BrowserSecureAttemptImpl::route_changed(RouteInfo value) {
    if (!active || failed)
        return;
    observed_route = value;
    callbacks.on_route(value);
}
BrowserTransportCallbacks BrowserSecureAttemptImpl::transport_callbacks() {
    auto weak = weak_from_this();
    return {[weak](std::string value) {
                if (auto self = weak.lock())
                    self->description(std::move(value));
            },
            [weak](std::string value, std::string mid) {
                if (auto self = weak.lock())
                    self->candidate(std::move(value), std::move(mid));
            },
            [weak](BrowserTransportState value) {
                if (auto self = weak.lock())
                    self->state(value);
            },
            [weak](Bytes value) {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    self->attempt->on_frame(value);
            },
            [weak](RouteInfo value) {
                if (auto self = weak.lock())
                    self->route_changed(value);
            },
            [weak](BrowserTransportError error) {
                if (auto self = weak.lock())
                    self->fail(error == BrowserTransportError::Resource
                                   ? AttemptFailure::Resource
                                   : AttemptFailure::Transport);
            },
            [weak] {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    self->attempt->wake();
            },
            [weak](bool ok) {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    (void)self->attempt->description_applied(ok);
            },
            [weak](std::uint32_t op, bool ok) {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    (void)self->attempt->candidate_applied(op, ok);
            },
            [weak](Bytes value) {
                if (auto self = weak.lock(); self && self->active && !self->failed &&
                                             self->attempt->ready() && self->callbacks.on_datagram)
                    self->callbacks.on_datagram(std::move(value));
            }};
}
SignalingFlowCallbacks BrowserSecureAttemptImpl::signaling_callbacks() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                auto self = weak.lock();
                return self && self->active && !self->failed &&
                       self->callbacks.submit_signal(std::move(value));
            },
            [weak](ByteView value) {
                auto self = weak.lock();
                if (!self || !self->active || self->failed)
                    return false;
                auto sdp =
                    std::string_view(reinterpret_cast<const char *>(value.data()), value.size());
                if (!self->transport->set_remote_description(sdp))
                    return false;
                const auto attribute = self->config.binding.role == HandshakeRole::Offerer
                                           ? datagram_answer_attribute
                                           : datagram_offer_attribute;
                if (self->config.datagrams && advertises_datagrams(sdp, attribute))
                    self->transport->enable_datagrams();
                return true;
            },
            [weak](CandidateView value) {
                auto self = weak.lock();
                if (!self || !self->active || self->failed)
                    return false;
                auto operation = self->attempt->candidate_operation();
                return operation && *operation <= std::numeric_limits<std::uint32_t>::max() &&
                       self->transport->add_remote_candidate(static_cast<std::uint32_t>(*operation),
                                                             value.candidate, value.mid);
            },
            [](SignalingKind) { return true; }};
}
SecureAttemptCallbacks BrowserSecureAttemptImpl::secure_callbacks() {
    auto weak = weak_from_this();
    return {signaling_callbacks(),
            [weak](std::uint32_t limit) {
                if (auto self = weak.lock(); self && self->active && !self->failed) {
                    self->negotiated = limit;
                    self->callbacks.on_ready(limit);
                }
            },
            [weak](Bytes value) {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    self->callbacks.on_message(std::move(value));
            },
            [weak] {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    self->callbacks.on_writable();
            },
            [weak](AttemptFailure reason) {
                if (auto self = weak.lock())
                    self->fail(reason);
            }};
}
} // namespace p2pdc::detail
