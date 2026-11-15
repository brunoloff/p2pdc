#pragma once

namespace p2pdc::detail {
class ConnectionBackend;
class NativeAttempt;

// Internal deterministic fault-injection and observation seams. The caller
// uses the owner executor and must not retain the returned attempt past close.
[[nodiscard]] NativeAttempt *native_attempt_for_testing(ConnectionBackend &connection);
[[nodiscard]] bool native_status_queued_for_testing(ConnectionBackend &connection);
} // namespace p2pdc::detail
