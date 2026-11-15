#include "incoming_backend.hpp"

namespace p2pdc {
IncomingListener::IncomingListener(std::shared_ptr<detail::IncomingBackend> state)
    : state_(std::move(state)) {}
IncomingListener::~IncomingListener() { close(); }
IncomingState IncomingListener::state() const {
    return state_ ? state_->state() : IncomingState::Closed;
}
std::optional<Advertisement> IncomingListener::peer() const {
    return state_ ? state_->peer() : std::nullopt;
}
void IncomingListener::close() {
    if (state_)
        state_->close();
}
} // namespace p2pdc
