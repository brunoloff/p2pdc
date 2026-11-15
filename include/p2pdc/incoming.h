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
struct IncomingOptions {
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds poll_interval{100};
};
struct IncomingHandlers {
    // Both handlers are required and run asynchronously on the node executor.
    std::function<void(Advertisement)> on_request;
    std::function<void(IncomingError)> on_error;
};

// Single-shot, owner-executor only. A request identifies a verified peer but
// does not grant authorization. Close rejects any pending request and attempts
// to notify its peer; network delivery is not guaranteed. It suppresses
// undelivered callbacks. A lease may be reused after the listener is closed or
// accepted; successive listeners on that lease retain bounded replay history.
// Accepted means consent was consumed; connection setup can still fail. Poll
// state() because scheduler submission failure may prevent an error callback.
class IncomingListener {
  public:
    ~IncomingListener();
    IncomingListener(const IncomingListener &) = delete;
    IncomingListener &operator=(const IncomingListener &) = delete;
    [[nodiscard]] IncomingState state() const;
    [[nodiscard]] std::optional<Advertisement> peer() const;
    void close();

  private:
    friend class Node;
    explicit IncomingListener(std::shared_ptr<detail::IncomingBackend> state);
    std::shared_ptr<detail::IncomingBackend> state_;
};
using ListenResult = std::variant<std::unique_ptr<IncomingListener>, IncomingError>;
} // namespace p2pdc
