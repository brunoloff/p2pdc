#include "../src/discovery/native_public_incoming.hpp"
#include "../src/transport/native_public_connection.hpp"
#include "native_public_incoming_fixture.hpp"

TEST(PublicIncomingErrors, PreservesKnownListenerAndStoreFailures) {
    EXPECT_EQ(incoming_error(IncomingListenerError::ResourceLimit), IncomingError::ResourceLimit);
    EXPECT_EQ(incoming_error(IncomingListenerError::Callback), IncomingError::Callback);
    EXPECT_EQ(incoming_error(IncomingListenerError::Mailbox), IncomingError::Backend);
    EXPECT_EQ(incoming_error(StoreError::ResourceLimit), IncomingError::ResourceLimit);
    EXPECT_EQ(incoming_error(StoreError::AllocationFailed), IncomingError::ResourceLimit);
    EXPECT_EQ(incoming_error(StoreError::NotRunning), IncomingError::Backend);
}

TEST(PublicConnectionErrors, PreservesKnownStoreFailures) {
    EXPECT_EQ(connection_store_error(StoreError::ResourceLimit), ConnectError::ResourceLimit);
    EXPECT_EQ(connection_store_error(StoreError::AllocationFailed), ConnectError::ResourceLimit);
    EXPECT_EQ(connection_store_error(StoreError::NotRunning), ConnectError::Signaling);
    EXPECT_EQ(connection_store_error(StoreError::PermanentFailure), ConnectError::Signaling);
    EXPECT_EQ(connection_store_error(StoreError::InvalidKey), ConnectError::Backend);
}

TEST_F(NativePublicIncomingTest, AcceptsOneRequestAndExchangesBytes) {
    setup_sides();
    listen();
    auto collision = receiver.node->connect(*receiver.lease, *receiver.watch, *receiver.peer, {},
                                            scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(collision));
    std::get<std::unique_ptr<Connection>>(collision)->close();
    connect_sender();
    EXPECT_FALSE(sending->max_message_size().has_value());
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(incoming_errors.empty());
    EXPECT_EQ(*request, *receiver.peer);
    EXPECT_EQ(incoming->state(), IncomingState::Pending);
    auto accepted = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    EXPECT_FALSE(receiving->max_message_size().has_value());
    auto duplicate = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    EXPECT_EQ(std::get<ConnectError>(duplicate), ConnectError::InvalidPeer);
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->max_message_size(), 256U * 1024U);
    ASSERT_EQ(receiving->max_message_size(), sending->max_message_size());
    EXPECT_EQ(sending->send(Bytes(*sending->max_message_size() + 1)), SendStatus::TooLarge);
    ASSERT_EQ(sending->send(Bytes{std::byte{42}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !received.empty(); }));
    EXPECT_EQ(received.front(), (Bytes{std::byte{42}}));
    ASSERT_EQ(receiving->send(Bytes{std::byte{0}, std::byte{255}}), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !sent.empty(); }));
    EXPECT_EQ(sent.front(), (Bytes{std::byte{0}, std::byte{255}}));
    sending->close();
    receiving->close();
    EXPECT_FALSE(sending->max_message_size().has_value());
    EXPECT_FALSE(receiving->max_message_size().has_value());
}

TEST_F(NativePublicIncomingTest, NegotiatesDifferentMessageLimitsAndPreservesPendingRequest) {
    setup_sides();
    listen();
    ConnectOptions invalid;
    invalid.max_message_bytes = 0;
    EXPECT_THROW(static_cast<void>(sender.node->connect(*sender.lease, *sender.watch, *sender.peer,
                                                        {}, scheduler(), invalid, handlers(sent))),
                 std::invalid_argument);
    ConnectOptions sender_options;
    sender_options.max_message_bytes = 32768;
    connect_sender({}, sender_options);
    ASSERT_TRUE(advance([&] { return request.has_value(); }));
    IceConfig unsupported;
    unsupported.servers = {TurnServer{"bad host", 5349, "user", "secret", TurnTransport::Tls}};
    EXPECT_THROW(static_cast<void>(receiver.node->accept(*incoming, unsupported, scheduler(), {},
                                                         handlers(received))),
                 std::invalid_argument);
    EXPECT_EQ(incoming->state(), IncomingState::Pending);
    invalid.max_message_bytes = 8192;
    invalid.max_pending_bytes = 8191;
    EXPECT_THROW(static_cast<void>(receiver.node->accept(*incoming, {}, scheduler(), invalid,
                                                         handlers(received))),
                 std::invalid_argument);
    EXPECT_EQ(incoming->state(), IncomingState::Pending);
    ConnectOptions receiver_options;
    receiver_options.max_message_bytes = 8192;
    auto accepted =
        receiver.node->accept(*incoming, {}, scheduler(), receiver_options, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(accepted));
    receiving = std::move(std::get<std::unique_ptr<Connection>>(accepted));
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready) ||
               !connection_errors.empty();
    }));
    ASSERT_TRUE(connection_errors.empty());
    ASSERT_EQ(sending->max_message_size(), 8192U);
    ASSERT_EQ(receiving->max_message_size(), 8192U);
    EXPECT_EQ(sending->send(Bytes(8193)), SendStatus::TooLarge);
    EXPECT_EQ(receiving->send(Bytes(8193)), SendStatus::TooLarge);
    const Bytes payload(8192, std::byte{42});
    ASSERT_EQ(sending->send(payload), SendStatus::Accepted);
    ASSERT_TRUE(loop.until([&] { return !received.empty(); }));
    EXPECT_EQ(received.front(), payload);
}

TEST_F(NativePublicIncomingTest, ReopenedListenerSuppressesOldOfferAndAcceptsFreshAttempt) {
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    incoming->close();
    EXPECT_EQ(incoming->state(), IncomingState::Closed);
    auto rejected = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    EXPECT_EQ(std::get<ConnectError>(rejected), ConnectError::InvalidPeer);
    request.reset();
    auto retry =
        receiver.node->listen(*receiver.lease, *receiver.watch, scheduler(), {},
                              {[&](Advertisement peer) { request = std::move(peer); },
                               [&](IncomingError error) { incoming_errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(retry));
    auto reopened = std::move(std::get<std::unique_ptr<IncomingListener>>(retry));
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    ASSERT_TRUE(advance([&] {
        return std::chrono::steady_clock::now() >= deadline || request || !incoming_errors.empty();
    }));
    ASSERT_FALSE(request);
    ASSERT_TRUE(incoming_errors.empty());
    auto fresh = sender.node->connect(*sender.lease, *sender.watch, *sender.peer, {}, scheduler(),
                                      {}, handlers(sent));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(fresh));
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    EXPECT_EQ(reopened->state(), IncomingState::Pending);
    std::get<std::unique_ptr<Connection>>(fresh)->close();
}

TEST_F(NativePublicIncomingTest, ClosingPendingRequestNotifiesDialingPeer) {
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    incoming->close();
    EXPECT_EQ(incoming->state(), IncomingState::Closed);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    ASSERT_TRUE(advance([&] {
        return !connection_errors.empty() || std::chrono::steady_clock::now() >= deadline;
    }));
    ASSERT_FALSE(connection_errors.empty()) << "rejection was not received promptly";
    EXPECT_EQ(connection_errors.front(), ConnectError::Rejected);
    EXPECT_EQ(sending->state(), ConnectionState::Failed);
}

TEST_F(NativePublicIncomingTest, UnknownOfferOverflowDoesNotFailTheListener) {
    setup_sides();
    listen();
    Peer external(std::byte{9});
    const auto now = wall_seconds();
    const auto &target = *sender.peer;
    for (unsigned index = 1; index <= 17; ++index) {
        AttemptId attempt{};
        attempt[0] = static_cast<std::byte>(index);
        SignalingFields fields{SignalingKind::Offer,
                               external.recipient.instance,
                               {target.signing_public_key, target.fields.instance_id},
                               attempt,
                               0,
                               now,
                               now + 30,
                               Bytes{std::byte{1}}};
        auto wire = encode_signaling(*external.identity, fields, target.fields.recipient_public_key,
                                     *external.capability, {now, 60, 0, 32768});
        ASSERT_TRUE(std::holds_alternative<Bytes>(wire));
        ASSERT_TRUE(put(target.fields.signaling_inbox, std::get<Bytes>(wire)));
    }
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    ASSERT_TRUE(advance([&] { return std::chrono::steady_clock::now() >= deadline; }));
    EXPECT_TRUE(incoming_errors.empty());
    EXPECT_FALSE(request);
    EXPECT_EQ(incoming->state(), IncomingState::Listening);
}

TEST_F(NativePublicIncomingTest, ClosingDialNotifiesPendingListener) {
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    sending->close();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    ASSERT_TRUE(advance(
        [&] { return !incoming_errors.empty() || std::chrono::steady_clock::now() >= deadline; }));
    ASSERT_FALSE(incoming_errors.empty()) << "cancellation was not received promptly";
    EXPECT_EQ(incoming_errors.front(), IncomingError::Canceled);
    EXPECT_EQ(incoming->state(), IncomingState::Failed);
}

TEST_F(NativePublicIncomingTest, ClosingImmediatelyAfterAcceptNotifiesDialer) {
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
    auto accepted = std::move(std::get<std::unique_ptr<Connection>>(result));
    accepted->close();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    ASSERT_TRUE(advance([&] {
        return !connection_errors.empty() || std::chrono::steady_clock::now() >= deadline;
    }));
    ASSERT_FALSE(connection_errors.empty());
    EXPECT_EQ(connection_errors.front(), ConnectError::Canceled);
}

TEST_F(NativePublicIncomingTest, CatchesUpCancellationPublishedBeforeOffer) {
    setup_sides();
    Peer external(std::byte{9});
    publish_external(external);
    ASSERT_FALSE(HasFatalFailure());
    listen();
    publish_cancel(external, AttemptId{std::byte{12}});
    std::uint64_t expiry = 0;
    publish_short_offer(external, expiry);
    ASSERT_FALSE(HasFatalFailure());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    ASSERT_TRUE(advance(
        [&] { return !incoming_errors.empty() || std::chrono::steady_clock::now() >= deadline; }));
    ASSERT_FALSE(incoming_errors.empty()) << "prior cancellation was not found";
    EXPECT_EQ(incoming_errors.front(), IncomingError::Canceled);
    EXPECT_EQ(incoming->state(), IncomingState::Failed);
}

TEST_F(NativePublicIncomingTest, IgnoresCancellationForAnotherAttempt) {
    setup_sides();
    Peer external(std::byte{9});
    publish_external(external);
    ASSERT_FALSE(HasFatalFailure());
    listen();
    std::uint64_t expiry = 0;
    publish_short_offer(external, expiry);
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    publish_cancel(external, AttemptId{std::byte{13}});
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    ASSERT_TRUE(advance(
        [&] { return !incoming_errors.empty() || std::chrono::steady_clock::now() >= deadline; }));
    EXPECT_TRUE(incoming_errors.empty());
    EXPECT_EQ(incoming->state(), IncomingState::Pending);
}

TEST_F(NativePublicIncomingTest, RequestMayBeAcceptedInsideItsCallback) {
    setup_sides();
    auto listening = receiver.node->listen(
        *receiver.lease, *receiver.watch, scheduler(), {},
        {[&](Advertisement) {
             auto result =
                 receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
             ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
             receiving = std::move(std::get<std::unique_ptr<Connection>>(result));
         },
         [&](IncomingError error) { incoming_errors.push_back(error); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<IncomingListener>>(listening));
    incoming = std::move(std::get<std::unique_ptr<IncomingListener>>(listening));
    connect_sender();
    ASSERT_TRUE(advance([&] { return receiving || !incoming_errors.empty(); }));
    ASSERT_TRUE(receiving);
    ASSERT_TRUE(advance([&] {
        return (sending->state() == ConnectionState::Ready &&
                receiving->state() == ConnectionState::Ready) ||
               !connection_errors.empty();
    }));
    EXPECT_TRUE(connection_errors.empty());
    EXPECT_EQ(incoming->state(), IncomingState::Accepted);
}

TEST_F(NativePublicIncomingTest, PendingRequestTimesOutAndCannotBeAccepted) {
    setup_sides();
    IncomingOptions options;
    options.request_timeout = 50ms;
    options.poll_interval = 10ms;
    listen(options);
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    ASSERT_TRUE(advance([&] { return !incoming_errors.empty(); }));
    EXPECT_EQ(incoming_errors.front(), IncomingError::Timeout);
    EXPECT_EQ(incoming->state(), IncomingState::Failed);
    auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    EXPECT_EQ(std::get<ConnectError>(result), ConnectError::InvalidPeer);
}

TEST_F(NativePublicIncomingTest, ClosingSubscriptionInvalidatesPendingAcceptance) {
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    receiver.watch->close();
    auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    EXPECT_EQ(std::get<ConnectError>(result), ConnectError::InvalidPeer);
}

TEST_F(NativePublicIncomingTest, ShutdownClosesPendingRequest) {
    setup_sides();
    listen();
    connect_sender();
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    bool stopped = false;
    ASSERT_TRUE(receiver.node->shutdown([&](auto) { stopped = true; }));
    EXPECT_EQ(incoming->state(), IncomingState::Closed);
    auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    EXPECT_EQ(std::get<ConnectError>(result), ConnectError::NotReady);
    ASSERT_TRUE(loop.until([&] { return stopped; }));
}

TEST_F(NativePublicIncomingTest, ExpiredOfferCannotBeAcceptedBeforeConsentDeadline) {
    setup_sides();
    Peer external(std::byte{9});
    publish_external(external);
    ASSERT_FALSE(HasFatalFailure());
    listen();
    std::uint64_t expiry = 0;
    publish_short_offer(external, expiry);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_NE(expiry, 0U);
    ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
    ASSERT_TRUE(request);
    ASSERT_TRUE(advance([&] { return wall_seconds() >= expiry; }));
    EXPECT_EQ(incoming->state(), IncomingState::Pending);
    auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
    EXPECT_EQ(std::get<ConnectError>(result), ConnectError::InvalidPeer);
}

#ifdef P2PDC_TEST_TURN
namespace {
#include "native_public_incoming_turn_tests.hpp"
} // namespace
#endif

namespace {
#include "native_public_large_message_test.hpp"
} // namespace
