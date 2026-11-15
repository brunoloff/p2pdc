#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

struct RunnerQuotaState {
    explicit RunnerQuotaState(std::size_t maximum) : limit(maximum) {}
    const std::size_t limit;
    std::atomic<std::size_t> used{0};
};

class RunnerPermit {
  public:
    RunnerPermit(const RunnerPermit &) = delete;
    RunnerPermit &operator=(const RunnerPermit &) = delete;
    RunnerPermit(RunnerPermit &&) noexcept = default;
    RunnerPermit &operator=(RunnerPermit &&) = delete;
    ~RunnerPermit() {
        if (state_) {
            state_->used.fetch_sub(1);
        }
    }

  private:
    friend class RunnerQuota;
    explicit RunnerPermit(std::shared_ptr<RunnerQuotaState> state) : state_(std::move(state)) {}
    std::shared_ptr<RunnerQuotaState> state_;
};

class RunnerQuota {
  public:
    explicit RunnerQuota(std::size_t maximum)
        : state_(std::make_shared<RunnerQuotaState>(maximum)) {
        if (maximum == 0) {
            throw std::invalid_argument("runner quota must be positive");
        }
    }
    [[nodiscard]] std::optional<RunnerPermit> try_acquire() const {
        auto used = state_->used.load();
        do {
            if (used >= state_->limit) {
                return std::nullopt;
            }
        } while (!state_->used.compare_exchange_weak(used, used + 1));
        return RunnerPermit(state_);
    }
    [[nodiscard]] std::size_t used() const noexcept { return state_->used.load(); }

  private:
    std::shared_ptr<RunnerQuotaState> state_;
};

inline RunnerQuota default_runner_quota() {
    static const RunnerQuota quota(8);
    return quota;
}
} // namespace p2pdc::detail
