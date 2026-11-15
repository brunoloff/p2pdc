#include "connection_backend.hpp"

#include <utility>

namespace p2pdc {
Connection::Connection(std::shared_ptr<detail::ConnectionBackend> state)
    : state_(std::move(state)) {}
Connection::~Connection() { close(); }
ConnectionState Connection::state() const {
    return state_ ? state_->state() : ConnectionState::Closed;
}
RouteInfo Connection::route() const { return state_ ? state_->route() : RouteInfo{}; }
std::optional<std::uint32_t> Connection::max_message_size() const {
    return state_ ? state_->max_message_size() : std::nullopt;
}
SendStatus Connection::send(ByteView message) {
    return state_ ? state_->send(message) : SendStatus::Closed;
}
void Connection::close() {
    if (state_) {
        state_->close();
    }
}
} // namespace p2pdc
