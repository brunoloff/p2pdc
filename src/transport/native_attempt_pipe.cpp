#include "../discovery/signal_errors.hpp"
#include "../discovery/terminal_signal.hpp"
#include "native_attempt_impl.hpp"

#include <iostream>

namespace p2pdc::detail {

// Datagrams count only once the handshake has authenticated the peer behind the
// connection's DTLS session; earlier ones are dropped like any lost datagram.
void NativeAttempt::Impl::on_datagram(Bytes value) {
    if (authenticated && status == NativeAttemptState::Ready && callbacks.on_datagram)
        callbacks.on_datagram(std::move(value));
}

ChannelSend NativeAttempt::send_datagram(ByteView payload) {
    if (impl_->status == NativeAttemptState::Closed)
        return ChannelSend::Closed;
    if (impl_->status == NativeAttemptState::Failed)
        return ChannelSend::Failed;
    if (impl_->status != NativeAttemptState::Ready || !impl_->authenticated)
        return ChannelSend::NotReady;
    return impl_->link->send_datagram(payload);
}

bool NativeAttempt::datagrams_available() const {
    return impl_->status == NativeAttemptState::Ready && impl_->authenticated &&
           impl_->link->datagrams_available();
}

void NativeAttempt::Impl::cancel_remote() noexcept {
    try {
        const auto current = policy();
        SignalingFields fields{SignalingKind::Cancel, config.binding.local.instance,
                               config.binding.remote, config.binding.attempt,
                               cancel_sequence,       current.now,
                               expiry(current.now),   {}};
        (void)publish_terminal_signal(config.store, config.remote_mailbox, *config.identity,
                                      std::move(fields), config.binding.remote_box,
                                      *config.capability, current);
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: could not publish cancellation: " << error.what() << '\n';
    }
}

void NativeAttempt::Impl::signal_failure(bool transient, ConnectError reason) {
    if (status == NativeAttemptState::Failed || status == NativeAttemptState::Closed)
        return;
    if (!authenticated || !transient) {
        fail(reason);
        return;
    }
    if (signaling_unavailable)
        return;
    signaling_unavailable = true;
    executor([weak = weak_from_this()] {
        if (auto self = weak.lock(); self && self->pipe)
            self->pipe->close();
    });
}

void NativeAttempt::on_signaling_error(OutboxError error) {
    impl_->signal_failure(transient_signal_error(error), resource_signal_error(error)
                                                             ? ConnectError::ResourceLimit
                                                             : ConnectError::Signaling);
}
void NativeAttempt::Impl::on_catchup(bool success) {
    if (!success)
        signal_failure(true);
}
void NativeAttempt::on_signaling_catchup(bool success) { impl_->on_catchup(success); }

SignalPipeCallbacks NativeAttempt::Impl::pipe_callbacks() {
    auto weak = weak_from_this();
    return {[weak](Bytes value) {
                if (auto self = weak.lock())
                    self->on_record(std::move(value));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock())
                    self->signal_failure(
                        error == MailboxError::StoreFailure && detail == StoreError::NotRunning,
                        resource_signal_error(error, detail) ? ConnectError::ResourceLimit
                                                             : ConnectError::Signaling);
            },
            [weak](OutboxError error) {
                if (auto self = weak.lock())
                    self->signal_failure(transient_signal_error(error),
                                         resource_signal_error(error) ? ConnectError::ResourceLimit
                                                                      : ConnectError::Signaling);
            },
            [] {},
            [weak](bool success) {
                if (auto self = weak.lock())
                    self->on_catchup(success);
            }};
}
} // namespace p2pdc::detail
