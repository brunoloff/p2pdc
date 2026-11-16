#include "../src/discovery/native_incoming_listener.hpp"
#include "../src/discovery/pending_offers.hpp"
#include "../src/transport/native_incoming_config.hpp"
#include "native_attempt_fixture.hpp"

namespace {
class NativeIncomingListenerTest : public NativeAttemptTest {
  protected:
    std::uint64_t time = 100;
    std::vector<IncomingOffer> offers;
    std::vector<IncomingListenerError> errors;

    IncomingContext make_context(const Peer &local, const Advertisement &remote) {
        auto identity = std::make_shared<EndpointIdentity>();
        identity->signing = local.identity;
        identity->recipient = local.box;
        AdvertisementFields fields{
            local.box->public_key(), local.recipient.instance, local.address, 1, 0, 100, 150};
        return {{AnnouncementEndpoint{identity, fields}, local.capability},
                {{remote}, local.capability, 60, 5}};
    }
    std::unique_ptr<NativeIncomingListener> listener(IncomingContext &context,
                                                     MemoryBudget budget = MemoryBudget(1048576)) {
        auto result = NativeIncomingListener::create(
            NativeStore(second, {32768, 128, 1048576}), context, [&] { return context; },
            [&] { return time; }, loop.executor(), std::move(budget),
            {[&](IncomingOffer offer) { offers.push_back(std::move(offer)); },
             [&](IncomingListenerError error) { errors.push_back(error); }});
        EXPECT_TRUE(std::holds_alternative<std::unique_ptr<NativeIncomingListener>>(result));
        return std::move(std::get<std::unique_ptr<NativeIncomingListener>>(result));
    }
    bool publish(const Peer &receiver, Bytes wire) {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        NativeStore store(first, {32768, 128, 1048576});
        if (store.put(receiver.address, std::move(wire),
                      [promise](bool ok) { promise->set_value(ok); }))
            return false;
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
};
} // namespace

TEST(PendingOffersTest, RetainsAndRemovesRequestsIndependently) {
    PendingOffers offers(2, 128);
    IncomingOffer first{}, second{};
    first.wire = Bytes(10);
    second.wire = Bytes(20);
    auto a = offers.add(std::move(first), PendingOffers::Clock::time_point{});
    auto b = offers.add(std::move(second), PendingOffers::Clock::time_point{} + 1s);
    ASSERT_TRUE(a && b);
    EXPECT_NE(*a, *b);
    EXPECT_EQ(offers.first_expired(PendingOffers::Clock::time_point{}), a);
    ASSERT_TRUE(offers.take(*a));
    EXPECT_EQ(offers.size(), 1U);
    EXPECT_TRUE(offers.find(*b));
    EXPECT_FALSE(offers.take(*a));
    EXPECT_EQ(offers.bytes(), 20U);
}
TEST(PendingOffersTest, EnforcesCountAndByteLimits) {
    EXPECT_THROW(PendingOffers(0, 1), std::invalid_argument);
    EXPECT_THROW(PendingOffers(1, 0), std::invalid_argument);
    PendingOffers offers(1, 5);
    IncomingOffer large{};
    large.wire = Bytes(6);
    EXPECT_FALSE(offers.add(std::move(large), {}));
    IncomingOffer first{};
    first.wire = Bytes(5);
    auto id = offers.add(std::move(first), {});
    ASSERT_TRUE(id);
    EXPECT_FALSE(offers.add(IncomingOffer{}, {}));
    ASSERT_TRUE(offers.take(*id));
    EXPECT_EQ(offers.bytes(), 0U);
    EXPECT_TRUE(offers.add(IncomingOffer{}, {}));
}
TEST(PendingOffersTest, FindsOnlyTheMatchingAuthenticatedAttempt) {
    PendingOffers offers(3, 1024);
    IncomingOffer a{}, b{};
    a.envelope.sender_signing_key[0] = std::byte{1};
    a.envelope.fields.sender_instance[0] = std::byte{2};
    a.envelope.fields.attempt_id[0] = std::byte{3};
    b.envelope = a.envelope;
    b.envelope.fields.attempt_id[0] = std::byte{4};
    auto cancel = a.envelope;
    auto first = offers.add(std::move(a), PendingOffers::Clock::time_point{} + 1s);
    auto second = offers.add(std::move(b), PendingOffers::Clock::time_point{} + 2s);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(offers.find_attempt(cancel), first);
    cancel.sender_signing_key[0] = std::byte{9};
    EXPECT_FALSE(offers.find_attempt(cancel));
    cancel.sender_signing_key[0] = std::byte{1};
    cancel.fields.sender_instance[0] = std::byte{9};
    EXPECT_FALSE(offers.find_attempt(cancel));
    cancel.fields.sender_instance[0] = std::byte{2};
    cancel.fields.attempt_id[0] = std::byte{4};
    EXPECT_EQ(offers.find_attempt(cancel), second);
    ASSERT_TRUE(offers.take(*second));
    EXPECT_FALSE(offers.find_attempt(cancel));
    EXPECT_EQ(offers.first_expired(PendingOffers::Clock::time_point{} + 1s), first);
}
TEST_F(NativeIncomingListenerTest, ReceivesVerifiedOfferAndSuppressesReplay) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    const auto wire = signed_seed(sender, receiver, SignalingKind::Offer);
    auto tampered = wire;
    tampered.back() ^= std::byte{1};
    ASSERT_TRUE(publish(receiver, std::move(tampered)));
    ASSERT_TRUE(publish(receiver, wire));
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || !offers.empty(); }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(offers.size(), 1U);
    EXPECT_EQ(offers.front().wire, wire);
    ASSERT_TRUE(publish(receiver, wire));
    ASSERT_TRUE(publish(
        receiver, signed_seed(sender, receiver, SignalingKind::Offer, AttemptId{std::byte{8}})));
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || offers.size() >= 2; }));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(offers.size(), 2U);
    watch->close();
    EXPECT_FALSE(watch->poll());
}
TEST_F(NativeIncomingListenerTest, ObservesRecordsWhileContinuingToAcceptOffers) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    std::vector<Bytes> records;
    auto created = NativeIncomingListener::create(
        NativeStore(second, {32768, 128, 1048576}), context, [&] { return context; },
        [&] { return time; }, loop.executor(), MemoryBudget(1048576),
        {[&](IncomingOffer offer) { offers.push_back(std::move(offer)); },
         [&](IncomingListenerError error) { errors.push_back(error); }, [] { return true; },
         [&](const Bytes &wire) { records.push_back(wire); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeIncomingListener>>(created));
    auto watch = std::move(std::get<std::unique_ptr<NativeIncomingListener>>(created));
    const auto cancel = signed_seed(sender, receiver, SignalingKind::Cancel);
    const auto offer = signed_seed(sender, receiver, SignalingKind::Offer);
    ASSERT_TRUE(publish(receiver, cancel));
    ASSERT_TRUE(publish(receiver, offer));
    ASSERT_TRUE(
        loop.until([&] { return !watch->poll() || (records.size() >= 2 && !offers.empty()); }));
    ASSERT_TRUE(errors.empty());
    EXPECT_NE(std::find(records.begin(), records.end(), cancel), records.end());
    EXPECT_NE(std::find(records.begin(), records.end(), offer), records.end());
    EXPECT_EQ(offers.size(), 1U);
}
TEST_F(NativeIncomingListenerTest, MailboxBudgetExhaustionDropsRecordsWithoutFailing) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context, MemoryBudget(1));
    ASSERT_TRUE(watch);
    ASSERT_TRUE(publish(receiver, signed_seed(sender, receiver, SignalingKind::Offer)));
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    ASSERT_TRUE(loop.until([&] {
        (void)watch->poll();
        return std::chrono::steady_clock::now() >= deadline;
    }));
    EXPECT_TRUE(watch->poll());
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(offers.empty());
}
TEST_F(NativeIncomingListenerTest, PausedOwnerDoesNotConsumeUnpresentedOffers) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    bool accepting = true;
    auto created = NativeIncomingListener::create(
        NativeStore(second, {32768, 128, 1048576}), context, [&] { return context; },
        [&] { return time; }, loop.executor(), MemoryBudget(1048576),
        {[&](IncomingOffer offer) {
             offers.push_back(std::move(offer));
             accepting = false;
         },
         [&](IncomingListenerError error) { errors.push_back(error); }, [&] { return accepting; }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeIncomingListener>>(created));
    auto watch = std::move(std::get<std::unique_ptr<NativeIncomingListener>>(created));
    ASSERT_TRUE(publish(receiver, signed_seed(sender, receiver, SignalingKind::Offer)));
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || !offers.empty(); }));
    ASSERT_EQ(offers.size(), 1U);
    auto second_offer =
        signed_seed(sender, receiver, SignalingKind::Offer, AttemptId{std::byte{8}});
    ASSERT_TRUE(publish(receiver, second_offer));
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    ASSERT_TRUE(loop.until([&] {
        (void)watch->poll();
        return std::chrono::steady_clock::now() >= deadline;
    }));
    ASSERT_EQ(offers.size(), 1U);
    accepting = true;
    ASSERT_TRUE(publish(receiver, second_offer));
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || offers.size() == 2; }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(offers.size(), 2U);
    EXPECT_EQ(offers.back().wire, second_offer);
}
TEST_F(NativeIncomingListenerTest, RetriesUnknownPeerWhenDiscoveryCatchesUp) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    const auto advertisement = verified_advertisement(sender);
    auto context = make_context(receiver, advertisement);
    context.remote.peers.clear();
    MemoryBudget budget(1048576);
    auto watch = listener(context, budget);
    ASSERT_TRUE(watch);
    const auto wire = signed_seed(sender, receiver, SignalingKind::Offer);
    ASSERT_TRUE(publish(receiver, wire));
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || watch->pending_offers() == 1; }));
    ASSERT_TRUE(errors.empty());
    ASSERT_TRUE(offers.empty());
    context.remote.peers.push_back(advertisement);
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || !offers.empty(); }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(offers.size(), 1U);
    EXPECT_EQ(offers.front().wire, wire);
    watch->close();
    EXPECT_EQ(watch->pending_offers(), 0U);
    EXPECT_EQ(budget.used(), 0U);
}
TEST_F(NativeIncomingListenerTest, UnknownPeerQueueIsBounded) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    context.remote.peers.clear();
    MemoryBudget budget(1048576);
    auto watch = listener(context, budget);
    ASSERT_TRUE(watch);
    for (unsigned index = 1; index <= 17; ++index) {
        AttemptId attempt{};
        attempt[0] = static_cast<std::byte>(index);
        ASSERT_TRUE(
            publish(receiver, signed_seed(sender, receiver, SignalingKind::Offer, attempt)));
    }
    ASSERT_TRUE(loop.until([&] { return !watch->poll(); }));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::ResourceLimit);
    EXPECT_TRUE(offers.empty());
    watch->close();
    EXPECT_EQ(budget.used(), 0U);
}
TEST_F(NativeIncomingListenerTest, ExpiredUnknownOffersReleaseReservations) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    context.local.endpoint.fields.expires_at = 1000;
    context.remote.peers.clear();
    MemoryBudget budget(1048576);
    auto watch = listener(context, budget);
    ASSERT_TRUE(watch);
    ASSERT_TRUE(publish(receiver, signed_seed(sender, receiver, SignalingKind::Offer)));
    ASSERT_TRUE(loop.until([&] { return !watch->poll() || watch->pending_offers() == 1; }));
    ASSERT_EQ(watch->pending_offers(), 1U);
    time = 171;
    ASSERT_TRUE(watch->poll());
    EXPECT_EQ(watch->pending_offers(), 0U);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(offers.empty());
    watch->close();
    EXPECT_EQ(budget.used(), 0U);
}
TEST_F(NativeIncomingListenerTest, DuplicateUnknownOffersDoNotExhaustQueue) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    context.remote.peers.clear();
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    const auto wire = signed_seed(sender, receiver, SignalingKind::Offer);
    for (unsigned index = 0; index < 20; ++index)
        ASSERT_TRUE(publish(receiver, wire));
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    ASSERT_TRUE(loop.until([&] {
        (void)watch->poll();
        return !errors.empty() || std::chrono::steady_clock::now() >= deadline;
    }));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(watch->pending_offers(), 1U);
    watch->close();
}

TEST_F(NativeIncomingListenerTest, RejectsChangedOrExpiredLease) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    context.local.endpoint.fields.signaling_inbox[0] ^= std::byte{1};
    EXPECT_FALSE(watch->poll());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::Context);
    EXPECT_FALSE(watch->is_open());
    watch->close();
    EXPECT_FALSE(watch->poll());
}

TEST_F(NativeIncomingListenerTest, ExpiredLeaseStopsTheListener) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    time = 150;
    EXPECT_FALSE(watch->poll());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::Context);
}

TEST_F(NativeIncomingListenerTest, ChangedCapabilityStopsTheListener) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    receiver.capability->at(0) ^= std::byte{1};
    EXPECT_FALSE(watch->poll());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::Context);
}

TEST_F(NativeIncomingListenerTest, ReplacedIdentityStopsTheListener) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    *receiver.identity = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{9})));
    EXPECT_FALSE(watch->poll());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::Context);
}

TEST_F(NativeIncomingListenerTest, ClockRegressionStopsTheListener) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    time = 101;
    ASSERT_TRUE(watch->poll());
    time = 100;
    EXPECT_FALSE(watch->poll());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::Clock);
}

TEST_F(NativeIncomingListenerTest, SnapshotAllocationFailureIsResourceError) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto result = NativeIncomingListener::create(
        NativeStore(second, {32768, 128, 1048576}), context,
        []() -> std::optional<IncomingContext> { throw std::bad_alloc(); }, [&] { return time; },
        loop.executor(), MemoryBudget(1048576),
        {[](IncomingOffer) {}, [&](IncomingListenerError error) { errors.push_back(error); }});
    auto watch = std::move(std::get<std::unique_ptr<NativeIncomingListener>>(result));
    EXPECT_FALSE(watch->poll());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::ResourceLimit);
}

TEST_F(NativeIncomingListenerTest, ThrowingOfferHandlerFailsClosed) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto result = NativeIncomingListener::create(
        NativeStore(second, {32768, 128, 1048576}), context, [&] { return context; },
        [&] { return time; }, loop.executor(), MemoryBudget(1048576),
        {[](IncomingOffer) { throw std::runtime_error("callback failed"); },
         [&](IncomingListenerError error) { errors.push_back(error); }});
    auto watch = std::move(std::get<std::unique_ptr<NativeIncomingListener>>(result));
    ASSERT_TRUE(publish(receiver, signed_seed(sender, receiver, SignalingKind::Offer)));
    ASSERT_TRUE(loop.until([&] { return !watch->poll(); }));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), IncomingListenerError::Callback);
    EXPECT_FALSE(watch->is_open());
}

TEST_F(NativeIncomingListenerTest, ApprovedOfferConnectsAndExchangesBytes) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto context = make_context(receiver, verified_advertisement(sender));
    auto watch = listener(context);
    ASSERT_TRUE(watch);
    auto outgoing = sender.config(receiver, first, HandshakeRole::Offerer);
    sender.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(outgoing), loop.executor(), sender.callbacks())));
    ASSERT_TRUE(loop.until([&] {
        (void)sender.attempt->poll_start();
        return !watch->poll() || !offers.empty() || sender.failed;
    }));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(offers.size(), 1U);
    ConnectionAdmission admission{
        second, context.local.endpoint, context.remote.peers.front(), context.local.capability, 60,
        5};
    auto incoming = make_native_incoming_config(admission, offers.front(), {},
                                                MemoryBudget(4194304), [] { return 100; });
    receiver.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(incoming), loop.executor(), receiver.callbacks())));
    ASSERT_TRUE(loop.until([&] {
        (void)sender.attempt->poll_start();
        (void)receiver.attempt->poll_start();
        return (sender.ready && receiver.ready) || sender.failed || receiver.failed;
    }));
    ASSERT_EQ(sender.ready, 1);
    ASSERT_EQ(receiver.ready, 1);
    ASSERT_EQ(sender.attempt->send(Bytes{std::byte{42}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !receiver.messages.empty(); }));
    EXPECT_EQ(receiver.messages.front(), (Bytes{std::byte{42}}));
    sender.attempt->close();
    receiver.attempt->close();
    watch->close();
}
