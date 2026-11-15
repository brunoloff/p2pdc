#pragma once

TEST_F(NativePublicOutgoingTest, PublicConnectionReportsTurnRelay) {
    Peer remote(std::byte{2});
    start_node(remote);
    ASSERT_TRUE(publish_remote(remote));
    ASSERT_TRUE(
        advance_until([&] { return published && remote_advertisement && local_advertisement; }));
    open_inbox(remote);
    std::vector<Bytes> messages;
    std::vector<RouteInfo> routes;
    std::vector<ConnectError> errors;
    ConnectionHandlers handlers{[](ConnectionState) {},
                                [&](RouteInfo route) { routes.push_back(route); },
                                [&](Bytes bytes) { messages.push_back(std::move(bytes)); }, [] {},
                                [&](ConnectError error) { errors.push_back(error); }};
    auto result = node->connect(*lease, *watch, *remote_advertisement, public_turn_config(true),
                                scheduler(), {}, handlers);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    auto connection = std::move(std::get<std::unique_ptr<Connection>>(result));
    auto offer = find_public_offer(remote);
    ASSERT_TRUE(offer);
    start_turn_responder(remote, *offer);
    ASSERT_TRUE(remote.attempt);
    ASSERT_TRUE(advance_until([&] {
        (void)remote.attempt->poll_start();
        return (connection->state() == ConnectionState::Ready && remote.ready && !routes.empty()) ||
               !errors.empty() || remote.failed;
    }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(connection->state(), ConnectionState::Ready);
    EXPECT_EQ(routes.back().current, RouteKind::TurnRelayed);
    EXPECT_EQ(routes.back().turn_history, TurnHistory::Used);
    ASSERT_TRUE(routes.back().selected_pair);
    EXPECT_EQ(routes.back().selected_pair->local, CandidateKind::Relayed);
    EXPECT_EQ(routes.back().selected_pair->remote, CandidateKind::Relayed);
    ASSERT_EQ(connection->send(Bytes{std::byte{42}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !remote.messages.empty(); }));
    EXPECT_EQ(remote.messages.front(), (Bytes{std::byte{42}}));
    ASSERT_EQ(remote.attempt->send(Bytes{std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !messages.empty(); }));
    EXPECT_EQ(messages.front(), (Bytes{std::byte{255}}));
    connection->close();
    EXPECT_EQ(connection->route().current, RouteKind::Unknown);
    EXPECT_EQ(connection->route().turn_history, TurnHistory::Used);
}

TEST_F(NativePublicOutgoingTest, PublicConnectionRejectsInvalidTurnCredentials) {
    Peer remote(std::byte{2});
    start_node(remote);
    ASSERT_TRUE(publish_remote(remote));
    ASSERT_TRUE(
        advance_until([&] { return published && remote_advertisement && local_advertisement; }));
    open_inbox(remote);
    std::vector<ConnectError> errors;
    ConnectionHandlers handlers{[](ConnectionState) {}, [](RouteInfo) {}, [](Bytes) {}, [] {},
                                [&](ConnectError error) { errors.push_back(error); }};
    ConnectOptions options;
    options.timeout = 3s;
    auto result = node->connect(*lease, *watch, *remote_advertisement, public_turn_config(false),
                                scheduler(), options, handlers);
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    auto connection = std::move(std::get<std::unique_ptr<Connection>>(result));
    auto offer = find_public_offer(remote);
    ASSERT_TRUE(offer);
    start_turn_responder(remote, *offer);
    ASSERT_TRUE(remote.attempt);
    ASSERT_TRUE(advance_until([&] {
        (void)remote.attempt->poll_start();
        return !errors.empty() || connection->state() == ConnectionState::Ready;
    }));
    EXPECT_FALSE(errors.empty());
    EXPECT_NE(connection->state(), ConnectionState::Ready);
    EXPECT_NE(connection->route().current, RouteKind::Direct);
}
