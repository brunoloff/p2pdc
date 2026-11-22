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

// Each pass of an application event loop must move many frames: a megabyte once took more than
// 256 passes, 4 KiB at a time.
TEST_F(NativePublicIncomingTest, SendsALargeMessageInFewEventLoopPasses) {
    constexpr std::uint32_t limit = 1024 * 1024;
    setup_sides();
    listen();
    ConnectOptions options;
    options.max_message_bytes = limit;
    options.max_pending_bytes = 2 * limit;
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
    ASSERT_TRUE(connection_errors.empty());
    Bytes payload(limit);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::byte>(i % 251);
    ASSERT_EQ(sending->send(payload), SendStatus::Accepted);
    const auto passes =
        loop.passes_until([&] { return !received.empty() || !connection_errors.empty(); }, 1000);
    ASSERT_TRUE(connection_errors.empty()) << static_cast<int>(connection_errors.front());
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received.front(), payload);
    EXPECT_LE(passes, 64U);
}

// The application gets one message an event-loop pass, fewer than arrive; the receiver holds the
// rest back instead of overflowing the application's queue and failing.
TEST_F(NativePublicIncomingTest, ManySmallMessagesSurviveAReceiverThatFallsBehind) {
    constexpr std::size_t total = 3000;
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(incoming_errors.empty());
    auto accepted = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    std::size_t next = 0;
    const auto message = [](std::size_t index) {
        return Bytes{static_cast<std::byte>(index), static_cast<std::byte>(index >> 8)};
    };
    (void)loop.passes_until(
        [&] {
            while (next < total && sending->send(message(next)) == SendStatus::Accepted)
                ++next;
            return received.size() == total || !connection_errors.empty();
        },
        20000, 1ms);
    ASSERT_TRUE(connection_errors.empty()) << static_cast<int>(connection_errors.front());
    ASSERT_EQ(received.size(), total);
    for (std::size_t index = 0; index < total; ++index)
        ASSERT_EQ(received[index], message(index));
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
