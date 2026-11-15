#pragma once

IceConfig public_turn_tls(RelayPolicy policy = RelayPolicy::RequireLocalRelay) {
    return {{TurnServer{public_turn_setting("P2PDC_TEST_TURN_HOST"), 53490,
                        public_turn_setting("P2PDC_TEST_TURN_USER"),
                        public_turn_setting("P2PDC_TEST_TURN_PASSWORD"), TurnTransport::Tls}},
            policy};
}

TEST_F(NativePublicIncomingTest, TlsAutomaticFallbackReportsRelay) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    auto ice = public_turn_tls(RelayPolicy::Automatic);
    connect_sender(ice);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto accepted = receiver.node->accept(*incoming, ice, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready && sending->route().selected_pair &&
                receiving->route().selected_pair) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->state(), ConnectionState::Ready);
    ASSERT_EQ(receiving->state(), ConnectionState::Ready);
    EXPECT_EQ(sending->route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(receiving->route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(sending->route().turn_history, TurnHistory::Used);
    EXPECT_EQ(receiving->route().turn_history, TurnHistory::Used);
    verify_relay_exchange_and_close();
}

TEST_F(NativePublicIncomingTest, TlsRelayConnectsAndExchangesAuthenticatedMessages) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    connect_sender(public_turn_tls());
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto accepted =
        receiver.node->accept(*incoming, public_turn_tls(), scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready && sending->route().selected_pair &&
                receiving->route().selected_pair) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->state(), ConnectionState::Ready);
    ASSERT_EQ(receiving->state(), ConnectionState::Ready);
    EXPECT_EQ(sending->route().selected_pair->local, CandidateKind::Relayed);
    EXPECT_EQ(receiving->route().selected_pair->local, CandidateKind::Relayed);
    verify_relay_exchange_and_close();
}

TEST_F(NativePublicIncomingTest, TlsRelayFailureDoesNotBecomeReady) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    ConnectOptions options;
    options.timeout = 5s;
    connect_sender(public_turn_tls(), options);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto accepted = receiver.node->accept(*incoming, public_turn_tls(), scheduler(), options,
                                          handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return sending->state() == ConnectionState::Failed &&
               receiving->state() == ConnectionState::Failed;
    }));
    EXPECT_FALSE(sending->route().selected_pair);
    EXPECT_FALSE(receiving->route().selected_pair);
}

TEST_F(NativePublicIncomingTest, BothPublicConnectionsReportForcedTurnRelay) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    connect_sender(public_turn(RelayPolicy::RequireLocalRelay));
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto accepted = receiver.node->accept(*incoming, public_turn(RelayPolicy::RequireLocalRelay),
                                          scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready && sending->route().selected_pair &&
                receiving->route().selected_pair) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->state(), ConnectionState::Ready);
    ASSERT_EQ(receiving->state(), ConnectionState::Ready);
    EXPECT_EQ(sending->route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(receiving->route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(sending->route().turn_history, TurnHistory::Used);
    EXPECT_EQ(receiving->route().turn_history, TurnHistory::Used);
    EXPECT_EQ(sending->route().selected_pair->local, CandidateKind::Relayed);
    EXPECT_EQ(sending->route().selected_pair->remote, CandidateKind::Relayed);
    EXPECT_EQ(receiving->route().selected_pair->local, CandidateKind::Relayed);
    EXPECT_EQ(receiving->route().selected_pair->remote, CandidateKind::Relayed);
    verify_relay_exchange_and_close();
    ASSERT_FALSE(HasFatalFailure());
}

TEST_F(NativePublicIncomingTest, AutomaticModeConnectsWhenPeerRequiresRelay) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    connect_sender(public_turn(RelayPolicy::Automatic));
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto accepted = receiver.node->accept(*incoming, public_turn(RelayPolicy::RequireLocalRelay),
                                          scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready && sending->route().selected_pair &&
                receiving->route().selected_pair) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->state(), ConnectionState::Ready);
    ASSERT_EQ(receiving->state(), ConnectionState::Ready);
    EXPECT_EQ(sending->route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(sending->route().selected_pair->remote, CandidateKind::Relayed);
    EXPECT_EQ(receiving->route().selected_pair->local, CandidateKind::Relayed);
    ASSERT_EQ(sending->send(Bytes{std::byte{7}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !received.empty(); }));
    EXPECT_EQ(received.front(), (Bytes{std::byte{7}}));
}

TEST_F(NativePublicIncomingTest, AutomaticModeUsesDirectWithBadTurnCredentials) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    connect_sender(public_turn(RelayPolicy::Automatic, false));
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto accepted = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready && sending->route().selected_pair) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->state(), ConnectionState::Ready);
    ASSERT_EQ(receiving->state(), ConnectionState::Ready);
    EXPECT_EQ(sending->route().current, RouteKind::Direct);
    ASSERT_EQ(sending->send(Bytes{std::byte{9}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !received.empty(); }));
    EXPECT_EQ(received.front(), (Bytes{std::byte{9}}));
}

TEST_F(NativePublicIncomingTest, ForcedRelayRejectsBadCredentialsWithoutReadiness) {
    setup_sides();
    ASSERT_FALSE(HasFatalFailure());
    listen();
    ASSERT_FALSE(HasFatalFailure());
    std::vector<ConnectionState> sender_states, receiver_states;
    auto sender_handlers = handlers(sent);
    sender_handlers.on_state = [&](ConnectionState state) { sender_states.push_back(state); };
    ConnectOptions options;
    options.timeout = 3s;
    auto result = sender.node->connect(*sender.lease, *sender.watch, *sender.peer,
                                       public_turn(RelayPolicy::RequireLocalRelay, false),
                                       scheduler(), options, std::move(sender_handlers));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    sending = std::move(std::get<std::unique_ptr<Connection>>(result));
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto receiver_handlers = handlers(received);
    receiver_handlers.on_state = [&](ConnectionState state) { receiver_states.push_back(state); };
    auto accepted = receiver.node->accept(*incoming, public_turn(RelayPolicy::RequireLocalRelay),
                                          scheduler(), options, std::move(receiver_handlers));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return sending->state() == ConnectionState::Failed &&
               receiving->state() == ConnectionState::Failed;
    }));
    EXPECT_EQ(std::count(sender_states.begin(), sender_states.end(), ConnectionState::Ready), 0);
    EXPECT_EQ(std::count(receiver_states.begin(), receiver_states.end(), ConnectionState::Ready),
              0);
}
