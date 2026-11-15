#include "p2pdc/discovery.h"

#include "discovery_backend.hpp"

#include <utility>

namespace p2pdc {
Subscription::Subscription(std::shared_ptr<detail::DiscoveryBackend> state)
    : state_(std::move(state)) {}
Subscription::~Subscription() { close(); }
void Subscription::close() {
    if (state_) {
        state_->close();
    }
}
bool Subscription::healthy() const { return state_ && state_->healthy(); }
bool Subscription::recovering() const { return state_ && state_->recovering(); }
bool Subscription::is_current(const Advertisement &value) const {
    return state_ && state_->is_current(value);
}

Lease::Lease(std::shared_ptr<detail::DiscoveryBackend> state) : state_(std::move(state)) {}
Lease::~Lease() { close(); }
void Lease::close() {
    if (state_) {
        state_->close();
    }
}
bool Lease::healthy() const { return state_ && state_->healthy(); }
} // namespace p2pdc
