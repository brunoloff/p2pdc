void Consumer::request(Advertisement peer) {
    auto &side = sides_[1];
    if (!side.peer || peer != *side.peer || listener_->state() != IncomingState::Pending) {
        finish(false);
        return;
    }
    if (!rejected_ice_keeps_request(*side.node, *listener_, scheduler_, handlers(1))) {
        finish(false);
        return;
    }
    if (test_accept_unsupported_) {
        const bool valid = accepts_after_unavailable(side);
        if (!valid || test_accept_unsupported_ == 2) {
            finish(valid);
            return;
        }
    }
    auto result = side.node->accept(*listener_, ice_, scheduler_, {}, handlers(1));
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection) {
        std::cerr << "accept error " << static_cast<int>(std::get<ConnectError>(result)) << '\n';
        finish(false);
        return;
    }
    side.connection = std::move(*connection);
}
bool Consumer::accepts_after_unavailable(Side &side) {
#ifdef __EMSCRIPTEN__
    set_test_webrtc_available(test_accept_unsupported_ == 2 ? 2 : 0);
    auto result = side.node->accept(*listener_, ice_, scheduler_, {}, handlers(1));
    set_test_webrtc_available(1);
    const auto *error = std::get_if<ConnectError>(&result);
    if (test_accept_unsupported_ == 2)
        return error && *error == ConnectError::Backend;
    return error && *error == ConnectError::Unsupported &&
           listener_->state() == IncomingState::Pending;
#else
    (void)side;
    return false;
#endif
}
