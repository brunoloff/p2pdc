#include "../discovery/signal_errors.hpp"
#include "browser_mailbox_attempt_impl.hpp"

#include <iostream>
#include <utility>

namespace p2pdc::detail {
bool BrowserMailboxAttemptImpl::submit(Bytes value) {
    if (!active || failed)
        return false;
    if (unavailable)
        return true;
    return pipe && pipe->send(std::move(value)) == SignalingQueueResult::Accepted && !failed;
}
void BrowserMailboxAttemptImpl::record(Bytes value) {
    if (!active || failed || !started || unavailable)
        return;
    const auto result = secure->receive(value);
    if (result == FlowResult::Failed || result == FlowResult::Closed)
        fail(AttemptFailure::Signaling);
}
void BrowserMailboxAttemptImpl::signal_failure(bool temporary, AttemptFailure reason) {
    if (!active || failed || unavailable)
        return;
    if (!authenticated || !temporary) {
        fail(reason);
        return;
    }
    unavailable = true;
    auto weak = weak_from_this();
    try {
        executor([weak] {
            if (auto self = weak.lock();
                self && self->active && !self->failed && self->unavailable) {
                self->pipe->close();
                self->callbacks.on_signaling_unavailable();
            }
        });
    } catch (...) {
        std::cerr << "p2pdc: signaling status delivery failed; close the attempt\n";
        fail(AttemptFailure::Resource);
    }
}
SignalPipeCallbacks BrowserMailboxAttemptImpl::pipe_callbacks() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                if (auto self = weak.lock())
                    self->record(std::move(value));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock())
                    self->signal_failure(
                        error == MailboxError::StoreFailure && detail == StoreError::NotRunning,
                        resource_signal_error(error, detail) ? AttemptFailure::Resource
                                                             : AttemptFailure::Signaling);
            },
            [weak](OutboxError error) {
                if (auto self = weak.lock())
                    self->signal_failure(transient_signal_error(error),
                                         resource_signal_error(error) ? AttemptFailure::Resource
                                                                      : AttemptFailure::Signaling);
            },
            [] {},
            [weak](bool ok) {
                if (auto self = weak.lock(); self && !ok)
                    self->signal_failure(true);
            }};
}
BrowserSecureCallbacks BrowserMailboxAttemptImpl::secure_callbacks() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                if (auto self = weak.lock())
                    return self->submit(std::move(value));
                return false;
            },
            [weak](std::uint32_t limit) {
                if (auto self = weak.lock(); self && self->active && !self->failed) {
                    self->authenticated = true;
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
            [weak](RouteInfo value) {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    self->callbacks.on_route(value);
            },
            [weak](BrowserTransportState value) {
                if (auto self = weak.lock(); self && self->active && !self->failed)
                    self->callbacks.on_transport(value);
            },
            [weak](AttemptFailure reason) {
                if (auto self = weak.lock())
                    self->fail(reason);
            }};
}
} // namespace p2pdc::detail
