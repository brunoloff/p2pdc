#include "shutdown_coordinator.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace p2pdc::detail {
ShutdownCoordinator::ShutdownCoordinator(Executor executor) : gate_(std::move(executor)) {}

std::optional<std::uint64_t> ShutdownCoordinator::track(std::function<void()> stop) {
    if (!stop) {
        throw std::invalid_argument("shutdown coordinator requires a stop action");
    }
    if (closing_) {
        return std::nullopt;
    }
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("shutdown coordinator registration IDs exhausted");
    }
    const auto id = next_id_;
    tracked_.emplace(id, std::make_shared<std::function<void()>>(std::move(stop)));
    ++next_id_;
    return id;
}

void ShutdownCoordinator::retire(std::uint64_t id) { tracked_.erase(id); }

CallbackGate ShutdownCoordinator::gate() const { return gate_; }

bool ShutdownCoordinator::begin(std::function<void()> final_callback) {
    if (!final_callback) {
        throw std::invalid_argument("shutdown coordinator requires a final callback");
    }
    if (closing_) {
        return false;
    }
    closing_ = true;
    final_ = std::move(final_callback);
    return retry();
}

bool ShutdownCoordinator::retry() {
    if (!closing_ || closed_ || sweeping_) {
        return false;
    }
    sweeping_ = true;
    struct ResetSweep {
        bool &active;
        ~ResetSweep() { active = false; }
    } reset{sweeping_};
    std::vector<std::pair<std::uint64_t, std::shared_ptr<std::function<void()>>>> actions;
    actions.reserve(tracked_.size());
    for (const auto &[id, stop] : tracked_) {
        actions.emplace_back(id, stop);
    }
    for (const auto &[id, stop] : actions) {
        if (tracked_.contains(id)) {
            (*stop)();
        }
    }
    tracked_.clear();
    auto gate = gate_;
    if (!gate_.post([gate, final = final_]() mutable {
            if (!gate.close(std::move(final))) {
                throw std::logic_error("shutdown callback gate unexpectedly closed");
            }
        })) {
        throw std::logic_error("shutdown callback gate unexpectedly closed");
    }
    final_ = {};
    closed_ = true;
    return true;
}
} // namespace p2pdc::detail
