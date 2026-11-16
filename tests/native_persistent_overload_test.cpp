#include "native_public_incoming_fixture.hpp"

namespace {
class PersistentOverloadTest : public NativePublicIncomingTest {
  protected:
    std::vector<IncomingRequestId> ids;
    std::size_t overflows = 0;
    std::unique_ptr<Connection> other;

    void start_persistent(std::size_t limit) {
        IncomingOptions options;
        options.persistent = true;
        options.max_pending_requests = limit;
        IncomingHandlers callbacks;
        callbacks.on_error = [&](IncomingError error) { incoming_errors.push_back(error); };
        callbacks.on_request_identified = [&](IncomingRequestId id, Advertisement) {
            ids.push_back(id);
        };
        callbacks.on_overflow = [&] { ++overflows; };
        auto result = receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), options,
                                            std::move(callbacks));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(result));
        incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(result));
    }
    void dial_twice() {
        connect_sender();
        auto result = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {},
                                           scheduler(), {}, handlers(sent));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
        other = std::move(std::get<std::unique_ptr<Connection>>(result));
    }
};
} // namespace

TEST_F(PersistentOverloadTest, FullQueueRejectsNewOfferAndPreservesPendingRequest) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(start_persistent(1));
    ASSERT_NO_FATAL_FAILURE(dial_twice());
    ASSERT_TRUE(advance([&] { return (!ids.empty() && overflows) || !incoming_errors.empty(); }));
    ASSERT_TRUE(incoming_errors.empty());
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(incoming->state(), IncomingState::Listening);
    auto accepted =
        receiver.node->accept(*incoming, ids.front(), {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return std::find(connection_errors.begin(), connection_errors.end(),
                         ConnectError::Rejected) != connection_errors.end();
    }));
}

TEST_F(PersistentOverloadTest, ClosingListenerRejectsAllPendingRequests) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(start_persistent(2));
    ASSERT_NO_FATAL_FAILURE(dial_twice());
    ASSERT_TRUE(advance([&] { return ids.size() == 2 || !incoming_errors.empty(); }));
    ASSERT_EQ(ids.size(), 2U);
    incoming->close();
    EXPECT_FALSE(incoming->reject(ids[0]));
    EXPECT_FALSE(incoming->reject(ids[1]));
    ASSERT_TRUE(advance([&] {
        return std::count(connection_errors.begin(), connection_errors.end(),
                          ConnectError::Rejected) >= 2;
    }));
    EXPECT_EQ(incoming->state(), IncomingState::Closed);
}
