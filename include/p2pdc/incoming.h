#pragma once

#include "p2pdc/advertisement.h"
#include "p2pdc/scheduler.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace p2pdc {
namespace detail {
class IncomingBackend;
}
enum class IncomingError {
    NotReady,
    Unsupported,
    InvalidContext,
    ResourceLimit,
    Backend,
    Scheduler,
    Timeout,
    Callback,
    Canceled
};
enum class IncomingState { Listening, Pending, Accepted, Failed, Closed };
using IncomingRequestId = std::uint64_t;
struct IncomingOptions {
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds poll_interval{100};
    bool persistent = false;               // Opt in to request-ID callbacks.
    std::size_t max_pending_requests = 16; // 1..128; also subject to a byte budget.
};
struct IncomingHandlers {
    // on_error and the callback for the chosen mode are required. Callbacks
    // run asynchronously on the node executor and must not throw.
    std::function<void(Advertisement)> on_request;
    std::function<void(IncomingError)> on_error;
    std::function<void(IncomingRequestId, Advertisement)> on_request_identified{};
    // Optional: timeout or authenticated remote cancellation of this request.
    std::function<void(IncomingRequestId, IncomingError)> on_request_ended{};
    // Coalesced notification: one or more new offers were rejected at capacity.
    std::function<void()> on_overflow{};
};

// Single-shot by default; persistent mode delivers requests independently by ID.
// Owner-executor only. A request identifies a verified peer but
// does not grant authorization. Close rejects any pending request and attempts
// to notify its peer; network delivery is not guaranteed. It suppresses
// undelivered callbacks. A lease may be reused after the listener is closed or
// accepted; successive listeners on that lease retain bounded replay history.
// Accepted means consent was consumed; connection setup can still fail. Poll
// state() because scheduler submission failure may prevent an error callback.
// Persistent mode stays Listening while requests are pending. At capacity it
// rejects new offers best-effort and can report a coalesced on_overflow event.
// When the per-lease replay history (32 recent offers) or the table of offers
// from not-yet-discovered peers is full, new offers are dropped and the
// listener stays open; replay entries are never evicted early. Local failures
// such as allocation can still fail the listener with ResourceLimit and discard
// pending requests without notifying peers.
// Close rejects every pending request and suppresses undelivered callbacks.
class IncomingListener {
  public:
    ~IncomingListener();
    IncomingListener(const IncomingListener &) = delete;
    IncomingListener &operator=(const IncomingListener &) = delete;
    [[nodiscard]] IncomingState state() const;
    [[nodiscard]] std::optional<Advertisement> peer() const;
    // An ID is valid only for this listener and while its request remains pending.
    [[nodiscard]] std::optional<IncomingRequestId> request_id() const;
    // False means the ID is unknown, expired, or the listener is closed.
    [[nodiscard]] bool reject(IncomingRequestId request_id);
    void close();

  private:
    friend class Node;
    explicit IncomingListener(std::shared_ptr<detail::IncomingBackend> state);
    std::shared_ptr<detail::IncomingBackend> state_;
};
using ListenResult = std::variant<std::unique_ptr<IncomingListener>, IncomingError>;
} // namespace p2pdc
