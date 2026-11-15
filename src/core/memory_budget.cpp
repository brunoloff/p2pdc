#include "p2pdc/memory_budget.h"

#include <utility>

namespace p2pdc {

MemoryReservation::MemoryReservation(std::shared_ptr<detail::BudgetState> state, std::size_t bytes)
    : state_(std::move(state)), bytes_(bytes) {}

MemoryReservation::MemoryReservation(MemoryReservation &&other) noexcept
    : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

MemoryReservation &MemoryReservation::operator=(MemoryReservation &&other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

MemoryReservation::~MemoryReservation() { release(); }

std::size_t MemoryReservation::bytes() const { return bytes_; }

void MemoryReservation::release() noexcept {
    if (state_) {
        state_->used.fetch_sub(bytes_, std::memory_order_relaxed);
        state_.reset();
        bytes_ = 0;
    }
}

MemoryBudget::MemoryBudget(std::size_t limit)
    : state_(std::make_shared<detail::BudgetState>(limit)) {}

std::optional<MemoryReservation> MemoryBudget::try_reserve(std::size_t bytes) const {
    auto used = state_->used.load(std::memory_order_relaxed);
    do {
        if (bytes > state_->limit - used) {
            return std::nullopt;
        }
    } while (!state_->used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return MemoryReservation(state_, bytes);
}

std::size_t MemoryBudget::used() const { return state_->used.load(std::memory_order_relaxed); }

std::size_t MemoryBudget::limit() const { return state_->limit; }

} // namespace p2pdc
