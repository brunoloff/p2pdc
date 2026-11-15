TEST_F(NativePublicIncomingTest, ExchangesLargeFragmentedMessagesWithinConfiguredLimits) {
    constexpr std::uint32_t limit = 4 * 1024 * 1024;
    setup_sides();
    listen();
    ConnectOptions options;
    options.max_message_bytes = limit;
    options.max_pending_bytes = limit;
    connect_sender({}, options);
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(incoming_errors.empty());
    auto accepted = receiver.node->accept(*incoming, {}, scheduler(), options, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty()) << static_cast<int>(connection_errors.front());
    ASSERT_EQ(sending->max_message_size(), limit);
    ASSERT_EQ(receiving->max_message_size(), limit);
    EXPECT_EQ(sending->send(Bytes(limit + 1)), SendStatus::TooLarge);
    Bytes payload(limit);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::byte>(i % 251);
    ASSERT_EQ(sending->send(payload), SendStatus::Accepted);
    ASSERT_EQ(receiving->send(payload), SendStatus::Accepted);
    ASSERT_TRUE(loop.until(
        [&] { return (!received.empty() && !sent.empty()) || !connection_errors.empty(); }));
    ASSERT_TRUE(connection_errors.empty()) << static_cast<int>(connection_errors.front());
    ASSERT_EQ(received.size(), 1U);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(received.front(), payload);
    EXPECT_EQ(sent.front(), payload);
}

TEST_F(NativePublicIncomingTest, RejectsMessageLimitBeyondBound) {
    setup_sides();
    ConnectOptions options;
    options.max_message_bytes = 4 * 1024 * 1024 + 1;
    options.max_pending_bytes = options.max_message_bytes;
    EXPECT_THROW(static_cast<void>(sender.node->connect(*sender.lease, *sender.watch, *sender.peer,
                                                        {}, scheduler(), options, handlers(sent))),
                 std::invalid_argument);
}
