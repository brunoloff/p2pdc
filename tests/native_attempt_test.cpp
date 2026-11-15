#include "../src/discovery/signal_errors.hpp"
#include "native_attempt_fixture.hpp"

TEST(SignalCatchupErrors, DistinguishesResourceExhaustionFromOtherOutcomes) {
    EXPECT_TRUE(resource_catchup_error(CatchupResult{StoreError::ResourceLimit}));
    EXPECT_TRUE(resource_catchup_error(CatchupResult{StoreError::AllocationFailed}));
    EXPECT_FALSE(resource_catchup_error(CatchupResult{StoreError::NotRunning}));
    EXPECT_FALSE(resource_catchup_error(CatchupResult{CatchupError::RegistrationFailed}));
    EXPECT_FALSE(resource_catchup_error(CatchupResult{std::monostate{}}));
}

TEST_F(NativeAttemptTest, ConnectsThroughDhtAndAuthenticatesMessages) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto a_config = a.config(b, first, HandshakeRole::Offerer);
    MemoryBudget budget = a_config.budget;
    a.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(a_config, loop.executor(), a.callbacks())));
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(NativeAttempt::create(
        b.config(a, second, HandshakeRole::Answerer), loop.executor(), b.callbacks())));
    a_config.identity.reset();
    a_config.recipient_box.reset();
    a_config.capability.reset();
    a.identity.reset();
    a.box.reset();
    a.capability.reset();
    EXPECT_EQ(a.attempt->send(Bytes{std::byte{1}}), ChannelSend::NotReady);
    bool started_a = false, started_b = false;
    ASSERT_TRUE(loop.until([&] {
        if (!started_b)
            started_b = b.attempt->poll_start() == NativeStart::Started;
        if (!started_a)
            started_a = a.attempt->poll_start() == NativeStart::Started;
        return (a.ready && b.ready && !a.routes.empty()) || a.failed || b.failed;
    }));
    ASSERT_EQ(a.failed, 0);
    ASSERT_EQ(b.failed, 0);
    ASSERT_EQ(a.ready, 1);
    ASSERT_EQ(b.ready, 1);
    ASSERT_FALSE(a.routes.empty());
    EXPECT_EQ(a.routes.front().current, RouteKind::Direct);
    EXPECT_TRUE(a.routes.front().selected_pair);
    EXPECT_TRUE(a.routes.front().observed_at);
    EXPECT_EQ(a.routes.front().turn_history, TurnHistory::Unknown);
    EXPECT_EQ(a.attempt->state(), NativeAttemptState::Ready);
    EXPECT_EQ(a.attempt->poll_start(), NativeStart::AlreadyStarted);
    ASSERT_EQ(a.attempt->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !b.messages.empty(); }));
    EXPECT_EQ(b.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    ASSERT_TRUE(loop.until([&] { return budget.used() == 0; }));
    auto held = budget.try_reserve(budget.limit());
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(a.attempt->send(Bytes{std::byte{9}}), ChannelSend::Backpressure);
    const auto writable_before = a.writable;
    held->release();
    a.attempt->poll_health();
    ASSERT_TRUE(loop.until([&] { return a.writable > writable_before; }));
    ASSERT_EQ(a.attempt->send(Bytes{std::byte{9}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return b.messages.size() == 2; }));
    a.attempt->on_signaling_catchup(true);
    EXPECT_FALSE(a.attempt->signaling_unavailable());
    a.attempt->on_signaling_catchup(false);
    b.attempt->on_signaling_error(OutboxFailure::BackendFailed);
    b.attempt->on_signaling_error(StoreError::NotRunning);
    ASSERT_TRUE(a.attempt->signaling_unavailable());
    ASSERT_TRUE(b.attempt->signaling_unavailable());
    a.attempt->poll_health();
    b.attempt->poll_health();
    ASSERT_EQ(a.attempt->send(Bytes{std::byte{10}}), ChannelSend::Accepted);
    ASSERT_EQ(b.attempt->send(Bytes{std::byte{11}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return b.messages.size() == 3 && a.messages.size() == 1; }));
    EXPECT_EQ(a.messages.front(), (Bytes{std::byte{11}}));
    EXPECT_EQ(b.messages.back(), (Bytes{std::byte{10}}));
    a.attempt->on_signaling_error(PublishError::ResourceLimit);
    ASSERT_TRUE(loop.until([&] { return a.failed == 1; }));
    EXPECT_EQ(a.attempt->state(), NativeAttemptState::Failed);
    EXPECT_EQ(a.attempt->failure_reason(), ConnectError::ResourceLimit);
    a.attempt->close();
    EXPECT_EQ(a.attempt->state(), NativeAttemptState::Closed);
    EXPECT_EQ(a.attempt->poll_start(), NativeStart::Closed);
}

TEST_F(NativeAttemptTest, SignalingFailureBeforeAuthenticationFailsConnection) {
    Peer a(std::byte{1}), b(std::byte{2});
    a.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(NativeAttempt::create(
        a.config(b, first, HandshakeRole::Offerer), loop.executor(), a.callbacks())));
    a.attempt->on_signaling_catchup(false);
    ASSERT_TRUE(loop.until([&] { return a.failed == 1; }));
    EXPECT_FALSE(a.attempt->signaling_unavailable());
    EXPECT_EQ(a.attempt->state(), NativeAttemptState::Failed);
    EXPECT_EQ(a.attempt->failure_reason(), ConnectError::Signaling);
    EXPECT_EQ(a.attempt->send(Bytes{}), ChannelSend::Failed);
}

TEST_F(NativeAttemptTest, TimeoutFailsOnceAndSuppressesReady) {
    Peer a(std::byte{1}), b(std::byte{2});
    a.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(NativeAttempt::create(
        a.config(b, first, HandshakeRole::Offerer), loop.executor(), a.callbacks())));
    a.attempt->timeout();
    a.attempt->timeout();
    EXPECT_EQ(a.attempt->state(), NativeAttemptState::Failed);
    EXPECT_EQ(a.attempt->failure_reason(), ConnectError::Timeout);
    ASSERT_TRUE(loop.until([&] { return a.failed == 1; }));
    EXPECT_EQ(a.ready, 0);
    EXPECT_TRUE(a.routes.empty());
    EXPECT_EQ(a.attempt->poll_start(), NativeStart::Failed);
    EXPECT_EQ(a.attempt->send(Bytes{}), ChannelSend::Failed);
}

TEST_F(NativeAttemptTest, RejectsGateLimitsOutsideChannelCapacity) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto config = a.config(b, first, HandshakeRole::Offerer);
    config.gate_transport_limit = config.channel_limits.frames.max_message_size + 1;
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    config.gate_transport_limit = 139;
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    config.gate_transport_limit = 1024;
    config.application_limit = 1024;
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
}

TEST_F(NativeAttemptTest, RetainsCryptographicInputsUntilAttemptIsDestroyed) {
    Peer a(std::byte{1}), b(std::byte{2});
    std::weak_ptr<Identity> identity = a.identity;
    std::weak_ptr<RecipientKeyPair> box = a.box;
    std::weak_ptr<Bytes> capability = a.capability;
    auto result = NativeAttempt::create(a.config(b, first, HandshakeRole::Offerer), loop.executor(),
                                        a.callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeAttempt>>(result));
    auto attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(result));
    a.identity.reset();
    a.box.reset();
    a.capability.reset();
    EXPECT_FALSE(identity.expired());
    EXPECT_FALSE(box.expired());
    EXPECT_FALSE(capability.expired());
    attempt.reset();
    EXPECT_TRUE(identity.expired());
    EXPECT_TRUE(box.expired());
    EXPECT_TRUE(capability.expired());
}

TEST_F(NativeAttemptTest, RejectsMissingCryptographicInputs) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto config = a.config(b, first, HandshakeRole::Offerer);
    config.identity.reset();
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    config = a.config(b, first, HandshakeRole::Offerer);
    config.recipient_box.reset();
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    config = a.config(b, first, HandshakeRole::Offerer);
    config.capability.reset();
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    config = a.config(b, first, HandshakeRole::Offerer);
    config.capability = std::make_shared<Bytes>(31);
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
}

TEST_F(NativeAttemptTest, RejectsInvalidIceSettingsBeforeStarting) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto config = a.config(b, first, HandshakeRole::Offerer);
    config.ice.relay_policy = RelayPolicy::RequireLocalRelay;
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    config.ice.servers = {TurnServer{"relay.example", 0, "user", "secret", TurnTransport::Tcp}};
    EXPECT_THROW((NativeAttempt::create(config, loop.executor(), a.callbacks())),
                 std::invalid_argument);
}

TEST_F(NativeAttemptTest, RejectsInvalidAcceptedOfferAndWrongRole) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto wrong_role = a.config(b, first, HandshakeRole::Offerer);
    wrong_role.accepted_offer = Bytes(signaling_wire_overhead);
    EXPECT_THROW((NativeAttempt::create(wrong_role, loop.executor(), a.callbacks())),
                 std::invalid_argument);
    auto config = b.config(a, second, HandshakeRole::Answerer);
    config.accepted_offer = Bytes(signaling_wire_overhead);
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(config), loop.executor(), b.callbacks())));
    ASSERT_TRUE(loop.until([&] { return b.attempt->poll_start() == NativeStart::Failed; }));
    ASSERT_TRUE(loop.until([&] { return b.failed == 1; }));
    EXPECT_EQ(b.ready, 0);
    EXPECT_EQ(b.attempt->state(), NativeAttemptState::Failed);
}

TEST_F(NativeAttemptTest, RejectsSignedCandidateAsAcceptedOffer) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto config = b.config(a, second, HandshakeRole::Answerer);
    config.accepted_offer = signed_seed(a, b, SignalingKind::Candidate);
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(config), loop.executor(), b.callbacks())));
    ASSERT_TRUE(loop.until([&] { return b.attempt->poll_start() == NativeStart::Failed; }));
    ASSERT_TRUE(loop.until([&] { return b.failed == 1; }));
    EXPECT_EQ(b.ready, 0);
}

TEST_F(NativeAttemptTest, RejectsAcceptedOfferForDifferentAttempt) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto config = b.config(a, second, HandshakeRole::Answerer);
    config.accepted_offer = signed_seed(a, b, SignalingKind::Offer, AttemptId{std::byte{9}});
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(config), loop.executor(), b.callbacks())));
    ASSERT_TRUE(loop.until([&] { return b.attempt->poll_start() == NativeStart::Failed; }));
    ASSERT_TRUE(loop.until([&] { return b.failed == 1; }));
    EXPECT_EQ(b.ready, 0);
}

TEST_F(NativeAttemptTest, RejectsAcceptedOfferThatExpiredBeforeStartup) {
    Peer a(std::byte{1}), b(std::byte{2});
    auto config = b.config(a, second, HandshakeRole::Answerer);
    config.accepted_offer = signed_seed(a, b, SignalingKind::Offer);
    config.now = [] { return std::uint64_t{155}; };
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(config), loop.executor(), b.callbacks())));
    ASSERT_TRUE(loop.until([&] { return b.attempt->poll_start() == NativeStart::Failed; }));
    ASSERT_TRUE(loop.until([&] { return b.failed == 1; }));
    EXPECT_EQ(b.ready, 0);
}

TEST_F(NativeAttemptTest, ResponderLearnsAttemptFromVerifiedIncomingOffer) {
    Peer a(std::byte{1}), b(std::byte{2});
    const std::array peers{verified_advertisement(a)};
    std::vector<Bytes> records;
    bool mailbox_failed = false;
    auto inbox = listen(b, records, mailbox_failed);
    ASSERT_EQ(inbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(inbox->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(inbox->catch_up()));
    auto outgoing = a.config(b, first, HandshakeRole::Offerer);
    outgoing.binding.attempt = AttemptId{std::byte{55}, std::byte{33}};
    a.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(outgoing), loop.executor(), a.callbacks())));
    IncomingOfferResolver resolver({16, 60, 60, 5, 32768, 64});
    auto offer = wait_for_offer(a, b, resolver, peers, records, mailbox_failed);
    ASSERT_FALSE(mailbox_failed);
    ASSERT_EQ(a.failed, 0);
    ASSERT_TRUE(offer);
    EXPECT_EQ(offer->peer, peers.front());
    EXPECT_EQ(offer->envelope.fields.attempt_id, (AttemptId{std::byte{55}, std::byte{33}}));
    EXPECT_EQ(std::get<IncomingOfferError>(
                  resolver.resolve(offer->wire, *b.box, b.recipient, *b.capability, peers, 100)),
              IncomingOfferError::Duplicate);
    ASSERT_TRUE(publish_conflicting_offer(a, b, *offer));
    auto config = responder_config(b, *offer);
    config.accepted_offer = offer->wire;
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(config), loop.executor(), b.callbacks())));
    ASSERT_TRUE(loop.until([&] {
        (void)b.attempt->poll_start();
        return (a.ready && b.ready) || a.failed || b.failed;
    }));
    ASSERT_EQ(a.failed, 0);
    ASSERT_EQ(b.failed, 0);
    ASSERT_EQ(a.ready, 1);
    ASSERT_EQ(b.ready, 1);
    ASSERT_EQ(a.attempt->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !b.messages.empty(); }));
    EXPECT_EQ(b.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    inbox->close();
}
