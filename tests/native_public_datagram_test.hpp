namespace {
const Bytes datagram_payload{std::byte{7}, std::byte{8}, std::byte{9}};

struct DatagramInboxes {
    std::vector<Bytes> to_sender, to_receiver;
};
} // namespace

class NativePublicDatagramTest : public NativePublicIncomingTest {
  protected:
    // Datagrams can still be on their way when a test ends.
    void TearDown() override {
        if (sending)
            sending->close();
        if (receiving)
            receiving->close();
        NativePublicIncomingTest::TearDown();
    }

    // Connects both sides with datagram handlers and waits until they are ready.
    void connect_pair(ConnectOptions offerer, ConnectOptions answerer) {
        setup_sides();
        listen();
        auto outgoing = handlers(sent);
        outgoing.on_datagram = [&](Bytes value) { pair.to_sender.push_back(std::move(value)); };
        auto result = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {},
                                           scheduler(), offerer, std::move(outgoing));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
        sending = std::move(std::get<std::unique_ptr<Connection>>(result));
        ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
        ASSERT_TRUE(incoming_errors.empty());
        auto inbound = handlers(received);
        inbound.on_datagram = [&](Bytes value) { pair.to_receiver.push_back(std::move(value)); };
        auto accepted =
            receiver.node->accept(*incoming, {}, scheduler(), answerer, std::move(inbound));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
        receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
        ASSERT_TRUE(advance([&] {
            return (sending->state() == ConnectionState::Ready &&
                    receiving->state() == ConnectionState::Ready) ||
                   !connection_errors.empty();
        }));
        ASSERT_TRUE(connection_errors.empty()) << static_cast<int>(connection_errors.front());
    }

    // Datagrams may be lost, so keep sending until one arrives.
    bool deliver(Connection &from, const std::vector<Bytes> &inbox) {
        return advance([&] {
            (void)from.send_datagram(datagram_payload);
            return !inbox.empty() || !connection_errors.empty();
        });
    }

    void expect_reliable_messages_work() {
        ASSERT_EQ(sending->send(Bytes{std::byte{42}}), SendStatus::Accepted);
        ASSERT_TRUE(loop.until([&] { return !received.empty() || !connection_errors.empty(); }));
        EXPECT_TRUE(connection_errors.empty());
        EXPECT_EQ(received, std::vector<Bytes>{Bytes{std::byte{42}}});
    }

    void expect_no_datagrams() {
        expect_reliable_messages_work();
        EXPECT_FALSE(sending->datagrams_available());
        EXPECT_FALSE(receiving->datagrams_available());
        EXPECT_EQ(sending->send_datagram(datagram_payload), SendStatus::NotReady);
        EXPECT_EQ(receiving->send_datagram(datagram_payload), SendStatus::NotReady);
        EXPECT_TRUE(pair.to_sender.empty());
        EXPECT_TRUE(pair.to_receiver.empty());
    }

    DatagramInboxes pair;
};

TEST_F(NativePublicDatagramTest, ExchangesDatagramsBothWays) {
    connect_pair({}, {});
    ASSERT_TRUE(advance([&] {
        return (sending->datagrams_available() && receiving->datagrams_available()) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_TRUE(deliver(*sending, pair.to_receiver));
    ASSERT_TRUE(deliver(*receiving, pair.to_sender));
    EXPECT_EQ(pair.to_receiver.front(), datagram_payload);
    EXPECT_EQ(pair.to_sender.front(), datagram_payload);
    expect_reliable_messages_work();
}

TEST_F(NativePublicDatagramTest, RejectsDatagramsOverTheSizeLimit) {
    connect_pair({}, {});
    EXPECT_EQ(sending->send_datagram(Bytes(max_datagram_bytes + 1)), SendStatus::TooLarge);
    ASSERT_TRUE(advance([&] { return sending->datagrams_available(); }));
    EXPECT_EQ(sending->send_datagram(Bytes(max_datagram_bytes + 1)), SendStatus::TooLarge);
    EXPECT_EQ(sending->send_datagram(Bytes(max_datagram_bytes)), SendStatus::Accepted);
}

TEST_F(NativePublicDatagramTest, AnAnswererWithoutSupportStillConnects) {
    ConnectOptions without;
    without.datagrams = false;
    connect_pair({}, without);
    expect_no_datagrams();
}

TEST_F(NativePublicDatagramTest, AnOffererWithoutSupportStillConnects) {
    ConnectOptions without;
    without.datagrams = false;
    connect_pair(without, {});
    expect_no_datagrams();
}

TEST_F(NativePublicDatagramTest, ClosedConnectionsRefuseDatagrams) {
    connect_pair({}, {});
    sending->close();
    EXPECT_FALSE(sending->datagrams_available());
    EXPECT_EQ(sending->send_datagram(datagram_payload), SendStatus::Closed);
}
