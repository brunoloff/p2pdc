#pragma once

#include "browser_mailbox_attempt.hpp"

namespace p2pdc::detail {
struct BrowserMailboxAttemptImpl : std::enable_shared_from_this<BrowserMailboxAttemptImpl> {
    BrowserMailboxAttemptImpl(BrowserMailboxConfig config, Executor executor,
                              BrowserMailboxCallbacks callbacks);
    [[nodiscard]] std::optional<StoreError> initialize();
    [[nodiscard]] SignalPipeCallbacks pipe_callbacks();
    [[nodiscard]] BrowserSecureCallbacks secure_callbacks();
    [[nodiscard]] bool submit(Bytes value);
    [[nodiscard]] bool apply_offer();
    void record(Bytes value);
    void signal_failure(bool transient, AttemptFailure reason = AttemptFailure::Signaling);
    void fail(AttemptFailure reason);
    void finish_failure(AttemptFailure reason);
    void cancel_remote() noexcept;
    void stop();
    void close();
    BrowserMailboxConfig config;
    Executor executor;
    BrowserMailboxCallbacks callbacks;
    std::unique_ptr<NativeSignalPipe> pipe;
    std::unique_ptr<BrowserSecureAttempt> secure;
    std::unique_ptr<std::weak_ptr<BrowserMailboxAttemptImpl>> emergency_cleanup;
    bool active = true, failed = false, started = false, authenticated = false;
    bool unavailable = false;
    AttemptFailure reason = AttemptFailure::Signaling;
};
} // namespace p2pdc::detail
