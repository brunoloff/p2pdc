#include "native_public_incoming_fixture.hpp"

namespace {
class PersistentGroupTest : public NativePublicIncomingTest {
  protected:
    Side third;
    std::vector<std::pair<IncomingRequestId, Advertisement>> requests;
    std::vector<Bytes> third_messages;
    std::unique_ptr<Connection> third_out, third_in;

    void start_listener() {
        IncomingOptions options;
        options.persistent = true;
        IncomingHandlers callbacks;
        callbacks.on_error = [&](IncomingError error) { incoming_errors.push_back(error); };
        callbacks.on_request_identified = [&](IncomingRequestId id, Advertisement peer) {
            requests.emplace_back(id, std::move(peer));
        };
        auto result = receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), options,
                                            std::move(callbacks));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(result));
        incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(result));
    }
    void find_receiver_from_third() {
        ASSERT_TRUE(advance([&] {
            for (const auto &peer : third.observed) {
                if (peer.signing_public_key == sender.peer->signing_public_key &&
                    std::holds_alternative<ConnectionAdmission>(
                        ConnectionAccess::resolve(*third.node, *third.lease, *third.watch, peer))) {
                    third.peer = peer;
                    return true;
                }
            }
            return false;
        }));
    }
    void TearDown() override {
        if (third.node) {
            bool stopped = false;
            if (third.node->shutdown([&](auto) { stopped = true; })) {
                EXPECT_TRUE(loop.until([&] { return stopped; }));
            }
            third.node.reset();
        }
        NativePublicIncomingTest::TearDown();
    }
    void establish_connections() {
        ASSERT_NO_FATAL_FAILURE(connect_sender());
        auto result = third.node->connect(*third.lease, *third.watch, *third.peer, {}, scheduler(),
                                          {}, handlers(third_messages));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
        third_out = std::move(std::get<std::unique_ptr<Connection>>(result));
        ASSERT_TRUE(advance([&] { return requests.size() == 2 || !incoming_errors.empty(); }));
        ASSERT_TRUE(incoming_errors.empty());
        ASSERT_EQ(requests.size(), 2U);
        EXPECT_NE(requests[0].first, requests[1].first);
        EXPECT_NE(requests[0].second.signing_public_key, requests[1].second.signing_public_key);
        for (const auto &[id, peer] : requests) {
            auto accepted =
                receiver.node->accept(*incoming, id, {}, scheduler(), {}, handlers(received));
            ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
            auto &connection =
                peer.signing_public_key == receiver.peer->signing_public_key ? receiving : third_in;
            connection = std::move(std::get<std::unique_ptr<Connection>>(accepted));
        }
        ASSERT_TRUE(receiving && third_in);
        ASSERT_TRUE(advance([&] {
            return (sending->state() == ConnectionState::Ready &&
                    receiving->state() == ConnectionState::Ready &&
                    third_out->state() == ConnectionState::Ready &&
                    third_in->state() == ConnectionState::Ready) ||
                   !connection_errors.empty();
        }));
        ASSERT_TRUE(connection_errors.empty());
    }
    void check_after_sender_shutdown() {
        bool stopped = false;
        ASSERT_TRUE(sender.node->shutdown([&](auto) { stopped = true; }));
        ASSERT_TRUE(loop.until([&] { return stopped; }));
        ASSERT_EQ(third_out->send(Bytes{std::byte{42}}), SendStatus::Accepted);
        ASSERT_TRUE(loop.until([&] { return !received.empty(); }));
        EXPECT_EQ(received.front(), (Bytes{std::byte{42}}));
        ASSERT_EQ(third_in->send(Bytes{std::byte{43}}), SendStatus::Accepted);
        ASSERT_TRUE(loop.until([&] { return !third_messages.empty(); }));
        EXPECT_EQ(third_messages.front(), (Bytes{std::byte{43}}));
        EXPECT_EQ(incoming->state(), IncomingState::Listening);
    }
};
} // namespace

TEST_F(PersistentGroupTest, DistinctPeersConnectAndRemainConnectedAfterFirstPeerLeaves) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(start_side(third));
    ASSERT_NO_FATAL_FAILURE(find_receiver_from_third());
    ASSERT_NO_FATAL_FAILURE(start_listener());
    ASSERT_NO_FATAL_FAILURE(establish_connections());
    ASSERT_NO_FATAL_FAILURE(check_after_sender_shutdown());
}
