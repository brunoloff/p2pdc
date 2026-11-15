#include "../discovery/signal_errors.hpp"
#include "native_attempt_impl.hpp"

namespace p2pdc::detail {

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
