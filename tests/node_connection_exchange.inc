void Consumer::check_exchange() {
    if (finishing_)
        return;
    if (sides_[0].ready && sides_[1].ready && !sent_) {
        sent_ = true;
        if (sides_[0].connection->send(Bytes(16384, std::byte{42})) != SendStatus::Accepted ||
            sides_[1].connection->send(Bytes{std::byte{0}, std::byte{255}}) !=
                SendStatus::Accepted) {
            finish(false);
            return;
        }
    }
    if (sides_[0].received && sides_[1].received && sides_[0].routed && sides_[1].routed) {
        if (!expect_turn_ && sides_[0].connection->route().current != RouteKind::Direct &&
            sides_[1].connection->route().current != RouteKind::Direct)
            return;
        if (!expect_turn_ && generation_ == 0) {
            repeat_exchange();
            return;
        }
        if (generation_ && (prior_[0]->state() != ConnectionState::Ready ||
                            prior_[1]->state() != ConnectionState::Ready)) {
            finish(false);
            return;
        }
        if (expect_turn_) {
            for (auto &side : sides_) {
                side.connection->close();
                if (side.connection->route().turn_history != TurnHistory::Used) {
                    finish(false);
                    return;
                }
            }
        }
        finish(true);
    }
}
void Consumer::repeat_exchange() {
    ++generation_;
    for (unsigned index = 0; index < sides_.size(); ++index) {
        auto &side = sides_[index];
        prior_[index] = std::move(side.connection);
        side.ready = side.received = side.routed = false;
    }
    listener_.reset();
    connecting_ = sent_ = false;
    check_discovery();
}
