#include "../src/transport/native_incoming_config.hpp"
#include "../src/transport/native_outgoing_config.hpp"
#include "native_attempt_fixture.hpp"

namespace {
class NativeOutgoingConfigTest : public NativeAttemptTest {};

ConnectionAdmission admission_for(const Peer &local, const Advertisement &remote,
                                  std::shared_ptr<dht::DhtRunner> runner) {
    auto identity = std::make_shared<EndpointIdentity>();
    identity->signing = local.identity;
    identity->recipient = local.box;
    AdvertisementFields fields{
        local.box->public_key(), local.recipient.instance, local.address, 1, 0, 100, 150};
    return {std::move(runner), {identity, fields}, remote, local.capability, 60, 5};
}
} // namespace

TEST_F(NativeOutgoingConfigTest, MapsAdmittedIdentityAndBounds) {
    Peer local(std::byte{1}), remote(std::byte{2});
    auto admission = admission_for(local, verified_advertisement(remote), first);
    IceConfig ice{{TurnServer{"127.0.0.1", 3478, "user", "password", TurnTransport::Tcp}},
                  RelayPolicy::RequireLocalRelay};
    MemoryBudget budget(4 * 1024 * 1024);
    auto config = make_native_outgoing_config(admission, ice, budget, [] { return 100; });
    EXPECT_EQ(config.local_mailbox, local.address);
    EXPECT_EQ(config.remote_mailbox, remote.address);
    EXPECT_EQ(config.binding.local, local.recipient);
    EXPECT_EQ(config.binding.remote, remote.recipient);
    EXPECT_EQ(config.binding.local_box, local.box->public_key());
    EXPECT_EQ(config.binding.remote_box, remote.box->public_key());
    EXPECT_EQ(config.binding.role, HandshakeRole::Offerer);
    EXPECT_FALSE(config.accepted_offer);
    EXPECT_EQ(config.ice.relay_policy, RelayPolicy::RequireLocalRelay);
    EXPECT_EQ(std::get<TurnServer>(config.ice.servers.front()).transport, TurnTransport::Tcp);
    EXPECT_LE(config.signaling_policy.max_payload_size + signaling_wire_overhead,
              config.pipe_limits.incoming.record_bytes);
    EXPECT_EQ(config.signaling_policy.clock_skew, admission.remote_clock_skew);
    auto reservation = budget.try_reserve(128);
    ASSERT_TRUE(reservation);
    EXPECT_EQ(config.receive_limits.budget.used(), 128U);
    auto next = make_native_outgoing_config(admission, {}, budget, [] { return 100; });
    EXPECT_NE(config.binding.attempt, next.binding.attempt);
}

TEST_F(NativeOutgoingConfigTest, RejectsInvalidOrExpiredAdmission) {
    Peer local(std::byte{1}), remote(std::byte{2});
    auto admission = admission_for(local, verified_advertisement(remote), first);
    EXPECT_THROW(static_cast<void>(
                     make_native_outgoing_config(admission, {}, MemoryBudget(4 * 1024 * 1024), {})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(make_native_outgoing_config(
                     admission, {}, MemoryBudget(4 * 1024 * 1024), [] { return 150; })),
                 std::invalid_argument);
    admission.local.fields.recipient_public_key[0] ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(make_native_outgoing_config(
                     admission, {}, MemoryBudget(4 * 1024 * 1024), [] { return 100; })),
                 std::invalid_argument);
}

TEST_F(NativeOutgoingConfigTest, RespectsAdmissionSkewAndMinimumBudget) {
    Peer local(std::byte{1}), remote(std::byte{2});
    auto admission = admission_for(local, verified_advertisement(remote), first);
    admission.remote.fields.issued_at = 110;
    admission.remote_clock_skew = 30;
    EXPECT_NO_THROW(static_cast<void>(
        make_native_outgoing_config(admission, {}, MemoryBudget(557056), [] { return 100; })));
    EXPECT_THROW(static_cast<void>(make_native_outgoing_config(admission, {}, MemoryBudget(557055),
                                                               [] { return 100; })),
                 std::invalid_argument);
    admission.remote_clock_skew = 5;
    EXPECT_THROW(static_cast<void>(make_native_outgoing_config(admission, {}, MemoryBudget(557056),
                                                               [] { return 100; })),
                 std::invalid_argument);
}

TEST_F(NativeOutgoingConfigTest, ConnectsAndExchangesAuthenticatedBytes) {
    Peer local(std::byte{1}), remote(std::byte{2});
    auto admission = admission_for(local, verified_advertisement(remote), first);
    auto outgoing = make_native_outgoing_config(admission, {}, MemoryBudget(4 * 1024 * 1024),
                                                [] { return 100; });
    auto incoming = remote.config(local, second, HandshakeRole::Answerer);
    incoming.binding.attempt = outgoing.binding.attempt;
    local.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(outgoing), loop.executor(), local.callbacks())));
    remote.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(incoming), loop.executor(), remote.callbacks())));
    ASSERT_TRUE(loop.until([&] {
        (void)local.attempt->poll_start();
        (void)remote.attempt->poll_start();
        return (local.ready && remote.ready) || local.failed || remote.failed;
    }));
    ASSERT_EQ(local.failed, 0);
    ASSERT_EQ(remote.failed, 0);
    ASSERT_EQ(local.ready, 1);
    ASSERT_EQ(remote.ready, 1);
    ASSERT_EQ(local.attempt->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !remote.messages.empty(); }));
    EXPECT_EQ(remote.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    Bytes fragmented(1024, std::byte{42});
    ASSERT_EQ(local.attempt->send(fragmented), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return remote.messages.size() == 2; }));
    EXPECT_EQ(remote.messages.back(), fragmented);
    EXPECT_EQ(local.attempt->send(Bytes(1025, std::byte{1})), ChannelSend::TooLarge);
    local.attempt->close();
    remote.attempt->close();
}

TEST_F(NativeOutgoingConfigTest, IncomingConfigBindsAndVerifiesTheExactOffer) {
    Peer local(std::byte{1}), remote(std::byte{2});
    auto admission = admission_for(local, verified_advertisement(remote), first);
    auto wire = signed_seed(remote, local, SignalingKind::Offer);
    IncomingOfferResolver resolver({16, 60, 60, 5, 32768, 64});
    const std::array peers{admission.remote};
    auto resolved =
        resolver.resolve(wire, *local.box, local.recipient, *local.capability, peers, 100);
    ASSERT_TRUE(std::holds_alternative<IncomingOffer>(resolved));
    auto offer = std::get<IncomingOffer>(std::move(resolved));
    auto make = [&](const IncomingOffer &value) {
        return make_native_incoming_config(admission, value, {}, MemoryBudget(4194304),
                                           [] { return 100; });
    };
    auto config = make(offer);
    EXPECT_EQ(config.binding.role, HandshakeRole::Answerer);
    EXPECT_EQ(config.binding.attempt, offer.envelope.fields.attempt_id);
    EXPECT_EQ(config.binding.remote, remote.recipient);
    ASSERT_TRUE(config.accepted_offer);
    EXPECT_EQ(*config.accepted_offer, wire);
    auto changed = offer;
    changed.wire.back() ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(make(changed)), std::invalid_argument);
    changed = offer;
    changed.peer.fields.signaling_inbox[0] ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(make(changed)), std::invalid_argument);
    changed = offer;
    changed.envelope.fields.attempt_id[0] ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(make(changed)), std::invalid_argument);
    changed = offer;
    changed.envelope.fields.sequence = 64;
    auto bad_sequence =
        encode_signaling(*remote.identity, changed.envelope.fields, local.box->public_key(),
                         *local.capability, {100, 60, 5, 32768});
    ASSERT_TRUE(std::holds_alternative<Bytes>(bad_sequence));
    changed.wire = std::get<Bytes>(std::move(bad_sequence));
    EXPECT_THROW(static_cast<void>(make(changed)), std::invalid_argument);
    changed = offer;
    changed.envelope.fields.expires_at = 101;
    auto short_wire =
        encode_signaling(*remote.identity, changed.envelope.fields, local.box->public_key(),
                         *local.capability, {100, 60, 5, 32768});
    ASSERT_TRUE(std::holds_alternative<Bytes>(short_wire));
    changed.wire = std::get<Bytes>(std::move(short_wire));
    EXPECT_THROW(static_cast<void>(make_native_incoming_config(
                     admission, changed, {}, MemoryBudget(4194304), [] { return 107; })),
                 std::invalid_argument);
    admission.remote = verified_advertisement(local);
    EXPECT_THROW(static_cast<void>(make(offer)), std::invalid_argument);
}

TEST_F(NativeOutgoingConfigTest, IncomingConfigConnectsWithPinnedOffer) {
    Peer sender(std::byte{1}), receiver(std::byte{2});
    auto outgoing =
        make_native_outgoing_config(admission_for(sender, verified_advertisement(receiver), first),
                                    {}, MemoryBudget(4194304), [] { return 100; });
    std::vector<Bytes> records;
    bool failed = false;
    auto inbox = listen(receiver, records, failed);
    ASSERT_EQ(inbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(inbox->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(inbox->catch_up()));
    sender.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(outgoing), loop.executor(), sender.callbacks())));
    IncomingOfferResolver resolver({16, 60, 60, 5, 32768, 64});
    const std::array peers{verified_advertisement(sender)};
    auto offer = wait_for_offer(sender, receiver, resolver, peers, records, failed);
    ASSERT_TRUE(offer);
    auto incoming = make_native_incoming_config(admission_for(receiver, peers[0], second), *offer,
                                                {}, MemoryBudget(4194304), [] { return 100; });
    receiver.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(
        NativeAttempt::create(std::move(incoming), loop.executor(), receiver.callbacks())));
    ASSERT_TRUE(loop.until([&] {
        (void)sender.attempt->poll_start();
        (void)receiver.attempt->poll_start();
        return (sender.ready && receiver.ready) || sender.failed || receiver.failed;
    }));
    ASSERT_EQ(sender.ready, 1);
    ASSERT_EQ(receiver.ready, 1);
    ASSERT_EQ(sender.attempt->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !receiver.messages.empty(); }));
    EXPECT_EQ(receiver.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    ASSERT_EQ(receiver.attempt->send(Bytes{std::byte{42}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !sender.messages.empty(); }));
    EXPECT_EQ(sender.messages.front(), (Bytes{std::byte{42}}));
    sender.attempt->close();
    receiver.attempt->close();
    inbox->close();
}
