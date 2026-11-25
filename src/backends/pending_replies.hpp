#pragma once

#include "backend_loop.hpp"
#include "backend_records.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace p2pdc::detail {

// Replies awaited from a server, each settled by its key or failed on timeout or a lost
// connection, as PendingReplies in websocket_channel.mjs. Loop thread only.
template <class Reply> class PendingReplies {
  public:
    using Settle = std::function<void(std::variant<Reply, BackendFailure>)>;
    explicit PendingReplies(std::shared_ptr<BackendLoop> loop) : loop_(std::move(loop)) {}
    PendingReplies(const PendingReplies &) = delete;
    PendingReplies &operator=(const PendingReplies &) = delete;
    ~PendingReplies() {
        for (const auto &[unused, entry] : waiting_)
            loop_->cancel(entry.timer);
    }

    void wait(const std::string &key, Settle settle,
              std::chrono::milliseconds timeout = backend_timeout) {
        fail(key, BackendFailure::Transient); // a reused key ends its earlier wait
        const auto timer = loop_->post_after(timeout, [this, key, alive = std::weak_ptr(alive_)] {
            if (alive.lock())
                fail(key, BackendFailure::Transient, false);
        });
        waiting_.emplace(key, Entry{std::move(settle), timer});
    }
    [[nodiscard]] bool has(const std::string &key) const { return waiting_.contains(key); }
    bool settle(const std::string &key, Reply reply) {
        auto entry = take(key, true);
        if (!entry)
            return false;
        entry->settle(std::move(reply));
        return true;
    }
    void fail(const std::string &key, BackendFailure failure, bool cancel_timer = true) {
        if (auto entry = take(key, cancel_timer))
            entry->settle(failure);
    }
    void fail_all(BackendFailure failure) {
        while (!waiting_.empty())
            fail(waiting_.begin()->first, failure);
    }

  private:
    struct Entry {
        Settle settle;
        BackendLoop::TimerId timer;
    };
    std::optional<Entry> take(const std::string &key, bool cancel_timer) {
        const auto found = waiting_.find(key);
        if (found == waiting_.end())
            return std::nullopt;
        auto entry = std::move(found->second);
        waiting_.erase(found);
        if (cancel_timer)
            loop_->cancel(entry.timer);
        return entry;
    }
    std::shared_ptr<BackendLoop> loop_;
    std::map<std::string, Entry> waiting_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace p2pdc::detail
