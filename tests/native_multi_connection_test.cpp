#include "native_public_incoming_fixture.hpp"

#include <tuple>

namespace {
struct ListenerGuard {
    IncomingListener *listener;
    ~ListenerGuard() { listener->close(); }
};
class NativeMultiConnectionTest : public NativePublicIncomingTest {
  protected:
    std::unique_ptr<Connection> second_out;
    void begin_two_attempts() {
        setup_sides();
        listen();
        connect_sender();
        auto next = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {},
                                         scheduler(), {}, handlers(sent));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(next));
        second_out = std::move(std::get<std::unique_ptr<Connection>>(next));
        ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
        ASSERT_TRUE(request);
        auto first = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(first));
        receiving = std::move(std::get<std::unique_ptr<Connection>>(first));
    }
};
} // namespace

TEST_F(NativeMultiConnectionTest, ConfigurableCapacityCountsRetainedAttemptsAndReleasesSlots) {
    max_connections = 1;
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(connect_sender());
    auto dial = [&] {
        return sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {}, scheduler(), {},
                                    handlers(sent));
    };
    auto full = dial();
    ASSERT_TRUE(std::holds_alternative<ConnectError>(full));
    EXPECT_EQ(std::get<ConnectError>(full), ConnectError::ResourceLimit);
    sending.reset();
    std::unique_ptr<Connection> reopened;
    ASSERT_TRUE(advance([&] {
        auto result = dial();
        if (auto *connection = std::get_if<std::unique_ptr<Connection>>(&result)) {
            reopened = std::move(*connection);
            return true;
        }
        EXPECT_EQ(std::get<ConnectError>(result), ConnectError::ResourceLimit);
        return false;
    }));
    EXPECT_TRUE(reopened);
}

TEST_F(NativeMultiConnectionTest, ConfiguredCapacityCanExceedEight) {
    max_connections = 10;
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    std::vector<std::unique_ptr<Connection>> connections;
    for (int i = 0; i < 10; ++i) {
        auto result = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {},
                                           scheduler(), {}, handlers(sent));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
        connections.push_back(std::move(std::get<std::unique_ptr<Connection>>(result)));
    }
    auto overflow = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {},
                                         scheduler(), {}, handlers(sent));
    ASSERT_TRUE(std::holds_alternative<ConnectError>(overflow));
    EXPECT_EQ(std::get<ConnectError>(overflow), ConnectError::ResourceLimit);
}

TEST_F(NativeMultiConnectionTest, IncomingCapacityFailurePreservesPendingRequest) {
    max_connections = 1;
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(listen());
    auto occupied = receiver.node->connect(*receiver.lease, *receiver.watch, *receiver.peer, {},
                                           scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(occupied));
    ASSERT_NO_FATAL_FAILURE(connect_sender());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto attempt = [&] {
        return receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    };
    auto full = attempt();
    ASSERT_TRUE(std::holds_alternative<ConnectError>(full));
    EXPECT_EQ(std::get<ConnectError>(full), ConnectError::ResourceLimit);
    EXPECT_EQ(incoming->state(), IncomingState::Pending);
    std::get<std::unique_ptr<Connection>>(occupied).reset();
    auto accepted = attempt();
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
}

TEST_F(NativeMultiConnectionTest, RequestSpecificAcceptanceRejectsWrongIdWithoutConsumingOffer) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(listen());
    EXPECT_FALSE(incoming->request_id());
    ASSERT_NO_FATAL_FAILURE(connect_sender());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto id = incoming->request_id();
    ASSERT_TRUE(id);
    auto wrong = receiver.node->accept(*incoming, *id + 1, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<ConnectError>(wrong));
    EXPECT_EQ(std::get<ConnectError>(wrong), ConnectError::InvalidPeer);
    EXPECT_EQ(incoming->request_id(), id);
    auto accepted = receiver.node->accept(*incoming, *id, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    EXPECT_FALSE(incoming->request_id());
}

TEST_F(NativeMultiConnectionTest, PersistentListenerHandlesConcurrentRequestsIndependently) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    IncomingOptions options;
    options.persistent = true;
    options.max_pending_requests = 2;
    std::vector<IncomingRequestId> ids;
    IncomingHandlers incoming_handlers;
    incoming_handlers.on_error = [&](IncomingError error) { incoming_errors.push_back(error); };
    incoming_handlers.on_request_identified = [&](IncomingRequestId id, Advertisement) {
        ids.push_back(id);
    };
    auto result = receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), options,
                                        std::move(incoming_handlers));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(result));
    incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(result));
    ListenerGuard guard{incoming.get()};
    ASSERT_NO_FATAL_FAILURE(connect_sender());
    auto other = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {}, scheduler(),
                                      {}, handlers(sent));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(other));
    second_out = std::move(std::get<std::unique_ptr<Connection>>(other));
    ASSERT_TRUE(advance([&] { return ids.size() == 2 || !incoming_errors.empty(); }));
    ASSERT_TRUE(incoming_errors.empty());
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_NE(ids[0], ids[1]);
    EXPECT_TRUE(incoming->reject(ids[0]));
    EXPECT_FALSE(incoming->reject(ids[0]));
    auto accepted =
        receiver.node->accept(*incoming, ids[1], {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    EXPECT_EQ(incoming->state(), IncomingState::Listening);
}

TEST_F(NativeMultiConnectionTest, PersistentCancellationLeavesOtherRequestPending) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    std::vector<IncomingRequestId> ids;
    std::vector<std::pair<IncomingRequestId, IncomingError>> ended;
    IncomingOptions options;
    options.persistent = true;
    IncomingHandlers callbacks;
    callbacks.on_error = [&](IncomingError error) { incoming_errors.push_back(error); };
    callbacks.on_request_identified = [&](IncomingRequestId id, Advertisement) {
        ids.push_back(id);
    };
    callbacks.on_request_ended = [&](IncomingRequestId id, IncomingError error) {
        ended.emplace_back(id, error);
    };
    auto listen_result = receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(),
                                               options, std::move(callbacks));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(listen_result));
    incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(listen_result));
    ListenerGuard guard{incoming.get()};
    ASSERT_NO_FATAL_FAILURE(connect_sender());
    auto other = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {}, scheduler(),
                                      {}, handlers(sent));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(other));
    second_out = std::move(std::get<std::unique_ptr<Connection>>(other));
    ASSERT_TRUE(advance([&] { return ids.size() == 2 || !incoming_errors.empty(); }));
    ASSERT_EQ(ids.size(), 2U);
    sending->close();
    ASSERT_TRUE(advance([&] { return !ended.empty() || !incoming_errors.empty(); }));
    ASSERT_EQ(ended.size(), 1U);
    EXPECT_EQ(ended[0].second, IncomingError::Canceled);
    auto remaining = ids[0] == ended[0].first ? ids[1] : ids[0];
    auto result =
        receiver.node->accept(*incoming, remaining, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    EXPECT_EQ(incoming->state(), IncomingState::Listening);
}

TEST_F(NativeMultiConnectionTest, PersistentTimeoutDoesNotStopListener) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    std::vector<IncomingRequestId> ids;
    std::vector<std::pair<IncomingRequestId, IncomingError>> ended;
    IncomingOptions options;
    options.persistent = true;
    options.request_timeout = 100ms;
    IncomingHandlers callbacks;
    callbacks.on_error = [&](IncomingError error) { incoming_errors.push_back(error); };
    callbacks.on_request_identified = [&](IncomingRequestId id, Advertisement) {
        ids.push_back(id);
    };
    callbacks.on_request_ended = [&](IncomingRequestId id, IncomingError error) {
        ended.emplace_back(id, error);
    };
    auto result = receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), options,
                                        std::move(callbacks));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(result));
    incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(result));
    ListenerGuard guard{incoming.get()};
    ASSERT_NO_FATAL_FAILURE(connect_sender());
    ASSERT_TRUE(advance([&] { return !ended.empty() || !incoming_errors.empty(); }));
    ASSERT_EQ(ids.size(), 1U);
    ASSERT_EQ(ended.size(), 1U);
    EXPECT_EQ(ended[0], std::make_pair(ids[0], IncomingError::Timeout));
    EXPECT_FALSE(incoming->reject(ids[0]));
    EXPECT_EQ(incoming->state(), IncomingState::Listening);
    auto next = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {}, scheduler(),
                                     {}, handlers(sent));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(next));
    second_out = std::move(std::get<std::unique_ptr<Connection>>(next));
    ASSERT_TRUE(advance([&] { return ids.size() == 2 || !incoming_errors.empty(); }));
    EXPECT_EQ(ids.size(), 2U);
}

TEST_F(NativeMultiConnectionTest, SimultaneousAttemptsShareLeasesWithoutLosingOffers) {
    ASSERT_NO_FATAL_FAILURE(begin_two_attempts());
    request.reset();
    auto next_listener =
        receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), {},
                              {[&](Advertisement peer) { request = std::move(peer); },
                               [&](IncomingError error) { incoming_errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(next_listener));
    auto listener = std::move(std::get<std::unique_ptr<IncomingListener>>(next_listener));
    auto duplicate = receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), {},
                                           {[](Advertisement) {}, [](IncomingError) {}});
    EXPECT_EQ(std::get<IncomingError>(duplicate), IncomingError::ResourceLimit);
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto next_in = receiver.node->accept(*listener, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(next_in));
    auto second_in = std::move(std::get<std::unique_ptr<Connection>>(next_in));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready &&
                second_out->state() == ConnectionState::Ready &&
                second_in->state() == ConnectionState::Ready) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->send(Bytes{std::byte{1}}), SendStatus::Accepted);
    ASSERT_EQ(second_out->send(Bytes{std::byte{2}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return received.size() == 2; }));
    EXPECT_NE(std::find(received.begin(), received.end(), Bytes{std::byte{1}}), received.end());
    EXPECT_NE(std::find(received.begin(), received.end(), Bytes{std::byte{2}}), received.end());
}

TEST_F(NativeMultiConnectionTest, ReopenedListenerRejectsChangedClockPolicy) {
    setup_sides();
    listen();
    incoming->close();
    DiscoveryCapability capability{};
    capability.fill(std::byte{8});
    WatchOptions changed;
    changed.clock_skew = 1;
    bool caught = false;
    auto result = receiver.node->watch(
        capability, scheduler(), changed,
        {[](Advertisement, PeerEvent) {}, [&](bool ok) { caught = ok; }, [](WatchError) {}});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Subscription>>(result));
    auto watch = std::move(std::get<std::unique_ptr<Subscription>>(result));
    ASSERT_TRUE(advance([&] { return caught; }));
    auto retry = receiver.node->listen(*receiver.lease, *watch, scheduler(), {},
                                       {[](Advertisement) {}, [](IncomingError) {}});
    ASSERT_TRUE(std::holds_alternative<IncomingError>(retry));
    EXPECT_EQ(std::get<IncomingError>(retry), IncomingError::InvalidContext);
}

namespace {
class NativeSimultaneousDialTest : public NativePublicIncomingTest {
  protected:
    std::optional<Advertisement> sender_request;
    std::unique_ptr<IncomingListener> sender_listener;
    std::unique_ptr<Connection> receiver_out, sender_in;
    void start_both() {
        setup_sides();
        listen();
        auto listener =
            sender.node->listen(*sender.lease, *sender.watch, scheduler(), {},
                                {[&](Advertisement peer) { sender_request = std::move(peer); },
                                 [&](IncomingError error) { incoming_errors.push_back(error); }});
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(listener));
        sender_listener = std::move(std::get<std::unique_ptr<IncomingListener>>(listener));
        connect_sender();
        auto outgoing = receiver.node->connect(*receiver.lease, *receiver.watch, *receiver.peer, {},
                                               scheduler(), {}, handlers(received));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(outgoing));
        receiver_out = std::move(std::get<std::unique_ptr<Connection>>(outgoing));
        ASSERT_TRUE(
            advance([&] { return (sender_request && request) || !incoming_errors.empty(); }));
        ASSERT_TRUE(sender_request && request);
    }
    bool sender_is_preferred() const {
        const auto &a = *receiver.peer;
        const auto &b = *sender.peer;
        return std::tie(a.signing_public_key, a.fields.instance_id) <
               std::tie(b.signing_public_key, b.fields.instance_id);
    }
    void resolve_both(bool sender_accepts_first) {
        auto accept_sender = [&] {
            return sender.node->accept(*sender_listener, {}, scheduler(), {}, handlers(sent));
        };
        auto accept_receiver = [&] {
            return receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
        };
        ConnectResult first = sender_accepts_first ? accept_sender() : accept_receiver();
        ConnectResult second = sender_accepts_first ? accept_receiver() : accept_sender();
        auto &sender_result = sender_accepts_first ? first : second;
        auto &receiver_result = sender_accepts_first ? second : first;
        auto &winner = sender_is_preferred() ? receiver_result : sender_result;
        auto &loser = sender_is_preferred() ? sender_result : receiver_result;
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(winner));
        ASSERT_TRUE(std::holds_alternative<ConnectError>(loser));
        EXPECT_EQ(std::get<ConnectError>(loser), ConnectError::Superseded);
        if (sender_is_preferred())
            receiving = std::move(std::get<std::unique_ptr<Connection>>(winner));
        else
            sender_in = std::move(std::get<std::unique_ptr<Connection>>(winner));
    }
    void check_winner() {
        auto *out = sender_is_preferred() ? sending.get() : receiver_out.get();
        auto *in = sender_is_preferred() ? receiving.get() : sender_in.get();
        ASSERT_NE(out, nullptr);
        ASSERT_NE(in, nullptr);
        ASSERT_TRUE(advance([&] {
            return out->state() == ConnectionState::Ready && in->state() == ConnectionState::Ready;
        }));
        ASSERT_EQ(out->send(Bytes{std::byte{3}}), SendStatus::Accepted);
        auto &messages = sender_is_preferred() ? received : sent;
        ASSERT_TRUE(loop.until([&] { return !messages.empty(); }));
        EXPECT_EQ(messages.front(), (Bytes{std::byte{3}}));
    }
};
} // namespace

TEST_F(NativeSimultaneousDialTest, ResolvesReceiverAcceptanceFirst) {
    ASSERT_NO_FATAL_FAILURE(start_both());
    ASSERT_NO_FATAL_FAILURE(resolve_both(false));
    ASSERT_NO_FATAL_FAILURE(check_winner());
}

TEST_F(NativeSimultaneousDialTest, ResolvesSenderAcceptanceFirst) {
    ASSERT_NO_FATAL_FAILURE(start_both());
    ASSERT_NO_FATAL_FAILURE(resolve_both(true));
    ASSERT_NO_FATAL_FAILURE(check_winner());
}

TEST_F(NativeSimultaneousDialTest, ClosedOppositeAttemptDoesNotBlockAcceptance) {
    ASSERT_NO_FATAL_FAILURE(start_both());
    receiver_out->close();
    auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(result));
    ASSERT_TRUE(advance([&] {
        return sending->state() == ConnectionState::Ready &&
               receiving->state() == ConnectionState::Ready;
    }));
}

TEST_F(NativeSimultaneousDialTest, FailedPreferredReplacementPreservesExistingConnection) {
    ASSERT_NO_FATAL_FAILURE(start_both());
    auto &listener = sender_is_preferred() ? sender_listener : incoming;
    auto &owner = sender_is_preferred() ? sender : receiver;
    auto &messages = sender_is_preferred() ? sent : received;
    // Close the preferred outgoing attempt so the nonpreferred offer can be accepted.
    auto &preferred_out = sender_is_preferred() ? sending : receiver_out;
    preferred_out->close();
    auto accepted = owner.node->accept(*listener, {}, scheduler(), {}, handlers(messages));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    auto existing = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    Scheduler broken = [](auto, auto) -> CancelTimer { throw std::runtime_error("timer failed"); };
    auto replacement = owner.node->connect(*owner.lease, *owner.watch, *owner.peer, {}, broken, {},
                                           handlers(messages));
    ASSERT_TRUE(std::holds_alternative<ConnectError>(replacement));
    EXPECT_EQ(std::get<ConnectError>(replacement), ConnectError::Scheduler);
    EXPECT_NE(existing->state(), ConnectionState::Failed);
    EXPECT_NE(existing->state(), ConnectionState::Closed);
}
