#include "../src/transport/native_public_connection_testing.hpp"

namespace {
class NativePublicSignalingTest : public NativePublicOutgoingTest {
  protected:
    Peer remote{std::byte{2}};
    std::shared_ptr<ConnectionBackend> connection;
    std::vector<ConnectionStatus> statuses;
    std::vector<ConnectionState> states;
    std::vector<ConnectError> errors;
    std::vector<Bytes> messages;
    std::deque<std::function<void()>> held_callbacks;
    bool hold_callbacks = false;

    void connect_owner() {
        start_node(remote);
        ASSERT_TRUE(publish_remote(remote));
        ASSERT_TRUE(advance_until(
            [&] { return published && remote_advertisement && local_advertisement; }));
        open_inbox(remote);
        auto admission = ConnectionAccess::resolve(*node, *lease, *watch, *remote_advertisement);
        ASSERT_TRUE(std::holds_alternative<ConnectionAdmission>(admission));
        auto executor = loop.executor();
        Executor delivery = [this, executor](std::function<void()> task) {
            if (hold_callbacks)
                held_callbacks.push_back(std::move(task));
            else
                executor(std::move(task));
        };
        ConnectionHandlers handlers{[&](auto state) { states.push_back(state); },
                                    [](RouteInfo) {},
                                    [&](Bytes value) { messages.push_back(std::move(value)); },
                                    [] {},
                                    [&](auto error) { errors.push_back(error); },
                                    [&](auto status) { statuses.push_back(status); }};
        connection = make_public_connection(executor, CallbackGate(delivery), scheduler(), {},
                                            std::move(handlers), [] { return true; });
        ASSERT_FALSE(connection->start(std::get<ConnectionAdmission>(admission), {}));
        auto offer = find_public_offer(remote);
        ASSERT_TRUE(offer);
        auto config = responder_config(remote, *offer);
        config.now = wall_seconds;
        config.accepted_offer = offer->wire;
        remote.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
            NativeAttempt::create(std::move(config), executor, remote.callbacks())));
        ASSERT_TRUE(advance_until([&] {
            (void)remote.attempt->poll_start();
            return (!states.empty() && states.back() == ConnectionState::Ready && remote.ready) ||
                   !errors.empty();
        }));
        ASSERT_TRUE(errors.empty());
    }
    void release_callbacks() {
        while (!held_callbacks.empty()) {
            auto task = std::move(held_callbacks.front());
            held_callbacks.pop_front();
            task();
        }
    }
};
TEST_F(NativePublicSignalingTest, ReportsOutageAndKeepsAuthenticatedChannelUsable) {
    connect_owner();
    ASSERT_FALSE(HasFatalFailure());
    auto *attempt = native_attempt_for_testing(*connection);
    ASSERT_NE(attempt, nullptr);
    attempt->on_signaling_error(OutboxFailure::BackendFailed);
    attempt->on_signaling_error(StoreError::NotRunning);
    ASSERT_TRUE(advance_until([&] { return !statuses.empty() || !errors.empty(); }));
    ASSERT_TRUE(errors.empty());
    EXPECT_EQ(statuses, (std::vector{ConnectionStatus::SignalingUnavailable}));
    EXPECT_EQ(connection->state(), ConnectionState::Ready);
    ASSERT_EQ(connection->send(Bytes{std::byte{42}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !remote.messages.empty(); }));
    EXPECT_EQ(remote.messages.front(), (Bytes{std::byte{42}}));
    ASSERT_EQ(remote.attempt->send(Bytes{std::byte{7}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !messages.empty(); }));
    EXPECT_EQ(messages.front(), (Bytes{std::byte{7}}));
    connection->close();
}

TEST_F(NativePublicSignalingTest, CloseSuppressesQueuedOutageNotification) {
    connect_owner();
    ASSERT_FALSE(HasFatalFailure());
    hold_callbacks = true;
    auto *attempt = native_attempt_for_testing(*connection);
    ASSERT_NE(attempt, nullptr);
    attempt->on_signaling_error(OutboxFailure::BackendFailed);
    ASSERT_TRUE(advance_until([&] { return native_status_queued_for_testing(*connection); }));
    ASSERT_FALSE(held_callbacks.empty());
    connection->close();
    release_callbacks();
    EXPECT_TRUE(statuses.empty());
    EXPECT_EQ(connection->state(), ConnectionState::Closed);
}
} // namespace
