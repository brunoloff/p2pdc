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
std::optional<IncomingRequestId> IncomingListener::request_id() const {
    return state_ ? state_->request_id() : std::nullopt;
}
bool IncomingListener::reject(IncomingRequestId request_id) {
    if (!state_ || !state_->peek_request(request_id))
        return false;
    state_->reject_request(request_id);
    return true;
}
void IncomingListener::close() {
    if (state_)
        state_->close();
}
} // namespace p2pdc
