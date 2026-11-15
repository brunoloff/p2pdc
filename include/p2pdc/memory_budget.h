#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

namespace p2pdc {

namespace detail {
struct BudgetState {
    explicit BudgetState(std::size_t maximum) : limit(maximum) {}
    const std::size_t limit;
    std::atomic<std::size_t> used{0};
};
} // namespace detail

class MemoryBudget;

class MemoryReservation {
  public:
    MemoryReservation(const MemoryReservation &) = delete;
    MemoryReservation &operator=(const MemoryReservation &) = delete;
    MemoryReservation(MemoryReservation &&other) noexcept;
    MemoryReservation &operator=(MemoryReservation &&other) noexcept;
    ~MemoryReservation();
    [[nodiscard]] std::size_t bytes() const;
    void release() noexcept;

  private:
    friend class MemoryBudget;
    MemoryReservation(std::shared_ptr<detail::BudgetState> state, std::size_t bytes);
    std::shared_ptr<detail::BudgetState> state_;
    std::size_t bytes_ = 0;
};

// Accounts explicitly reserved bytes, not unreserved allocator or backend overhead.
class MemoryBudget {
  public:
    explicit MemoryBudget(std::size_t limit);
    [[nodiscard]] std::optional<MemoryReservation> try_reserve(std::size_t bytes) const;
    [[nodiscard]] std::size_t used() const;
    [[nodiscard]] std::size_t limit() const;

  private:
    std::shared_ptr<detail::BudgetState> state_;
};

} // namespace p2pdc
