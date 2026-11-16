#include "native_discovery_watch_fixture.hpp"

TEST_F(NativeDiscoveryWatchTest, RetainsCredentialsAfterCallerReleasesThem) {
    std::weak_ptr<Identity> identity_lifetime = identity;
    std::weak_ptr<Bytes> capability_lifetime = capability;
    auto watched = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, {{4096, 16, 1024}, 4, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeDiscoveryWatch>>(watched));
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(watched));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(watch->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(watch->catch_up()));
    bool published = false;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, lease_config(),
        MemoryBudget(4096), loop.executor(), [this] { return time; },
        {[&](std::uint64_t) { published = true; }, [](AdvertisementLeaseError) { ADD_FAILURE(); }});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeAdvertisementLease>>(created));
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    identity.reset();
    capability.reset();
    EXPECT_FALSE(identity_lifetime.expired());
    EXPECT_FALSE(capability_lifetime.expired());
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published && !events.empty(); }));
    EXPECT_EQ(events.front(), AdvertisementEvent::Seen);
    lease.reset();
    watch.reset();
    EXPECT_TRUE(identity_lifetime.expired());
    EXPECT_TRUE(capability_lifetime.expired());
}

#include "native_windowed_lease_tests.hpp"

TEST_F(NativeDiscoveryWatchTest, DiscoversUpdatesAndExpiresVerifiedAdvertisements) {
    const auto first_record = advertisement(1, 100, 150);
    ASSERT_TRUE(publish(first_record));
    auto result = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, {{4096, 16, 1024}, 4, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeDiscoveryWatch>>(result));
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(result));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(watch->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(watch->catch_up()));
    ASSERT_TRUE(loop.until([&] { return caught_up.has_value() && !events.empty(); }));
    EXPECT_TRUE(*caught_up);
    EXPECT_EQ(events.front(), AdvertisementEvent::Seen);
    time = 101;
    ASSERT_TRUE(publish(advertisement(2, 101, 110)));
    ASSERT_TRUE(loop.until([&] { return events.size() == 2 || !errors.empty(); }));
    ASSERT_TRUE(errors.empty());
    EXPECT_EQ(events.back(), AdvertisementEvent::Updated);
    time = 115;
    ASSERT_TRUE(watch->tick());
    ASSERT_EQ(expired.size(), 1U);
    EXPECT_EQ(expired.front().fields.sequence, 2U);
    time = 114;
    EXPECT_FALSE(watch->tick());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), DiscoveryWatchError::ClockRegressed);
    EXPECT_FALSE(watch->is_open());
    watch->close();
    EXPECT_EQ(std::get<CatchupError>(watch->catch_up()), CatchupError::Closed);
}

TEST_F(NativeDiscoveryWatchTest, CatchupDeduplicatesRepeatedAdvertisementBytes) {
    auto result = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 2, 2048}), capability, {{4096, 16, 1024}, 4, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeDiscoveryWatch>>(result));
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(result));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(watch->registration().get(), 0U);
    const auto wire = advertisement(1, 100, 150);
    ASSERT_TRUE(publish(wire));
    ASSERT_TRUE(loop.until([&] { return !events.empty() || !errors.empty(); }));
    for (int i = 0; i < 2; ++i) {
        const auto previous = loop.submitted();
        ASSERT_TRUE(publish(wire));
        ASSERT_TRUE(loop.until([&] { return loop.submitted() > previous || !errors.empty(); }));
    }
    ASSERT_TRUE(errors.empty());
    ASSERT_TRUE(std::holds_alternative<std::monostate>(watch->catch_up()));
    ASSERT_TRUE(loop.until([&] { return caught_up.has_value() || !errors.empty(); }));
    ASSERT_TRUE(errors.empty());
    ASSERT_TRUE(caught_up.value_or(false));
    EXPECT_EQ(events, (std::vector{AdvertisementEvent::Seen}));
}

TEST(NativeDiscoveryWatchValidation, RejectsMissingCallbacksAndInvalidCapability) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, {1024, 16, 16384});
    DiscoveryWatchLimits limits{{1024, 4, 512}, 4, 60, 5};
    DiscoveryWatchCallbacks callbacks{
        [](Advertisement, AdvertisementEvent) {}, [](Advertisement) {},
        [](DiscoveryWatchError, std::optional<StoreError>) {}, [](bool) {}};
    EXPECT_THROW((NativeDiscoveryWatch::create(
                     store, {}, limits, MemoryBudget(2048), [](auto) {}, [] { return 100; }, {})),
                 std::invalid_argument);
    EXPECT_THROW(
        (NativeDiscoveryWatch::create(
            store, {}, limits, MemoryBudget(2048), [](auto) {}, [] { return 100; }, callbacks)),
        std::invalid_argument);
    EXPECT_EQ(std::get<CryptoError>(NativeDiscoveryWatch::create(
                  store, std::make_shared<Bytes>(), limits, MemoryBudget(2048), [](auto) {},
                  [] { return 100; }, callbacks)),
              CryptoError::InvalidLength);
    limits.mailbox.record_bytes = advertisement_wire_size - 1;
    EXPECT_THROW((NativeDiscoveryWatch::create(
                     store, std::make_shared<Bytes>(32), limits, MemoryBudget(2048), [](auto) {},
                     [] { return 100; }, callbacks)),
                 std::invalid_argument);
}

TEST_F(NativeDiscoveryWatchTest, IgnoresNewPeersAtCapacityWithoutEvictingOrFailing) {
    auto result = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, {{4096, 16, 1024}, 1, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeDiscoveryWatch>>(result));
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(result));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(watch->registration().get(), 0U);
    ASSERT_TRUE(publish(advertisement(1, 100, 150)));
    ASSERT_TRUE(loop.until([&] { return events.size() == 1; }));
    auto other = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    AdvertisementFields fields{RecipientPublicKey{std::byte{2}},
                               InstanceId{std::byte{9}},
                               SignalingInboxKey{std::byte{4}},
                               1,
                               0,
                               100,
                               150};
    auto wire = std::get<Bytes>(encode_advertisement(other, fields, *capability, {100, 60, 5}));
    ASSERT_TRUE(publish(wire));
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    ASSERT_TRUE(loop.until([&] { return std::chrono::steady_clock::now() >= deadline; }));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(events.size(), 1U);
    EXPECT_TRUE(watch->is_open());
    EXPECT_TRUE(watch->tick());
}

TEST_F(NativeDiscoveryWatchTest, CurrentPeerFailsClosedWhenClockRegresses) {
    const auto wire = advertisement(1, 100, 150);
    const auto peer =
        std::get<Advertisement>(decode_advertisement(wire, *capability, {100, 60, 5}));
    auto result = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, {{4096, 16, 1024}, 4, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(result));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(publish(wire));
    ASSERT_TRUE(loop.until([&] { return !events.empty(); }));
    EXPECT_TRUE(watch->is_current(peer));
    auto changed = peer;
    changed.fields.signaling_inbox[0] = std::byte{9};
    EXPECT_FALSE(watch->is_current(changed));
    time = 99;
    EXPECT_FALSE(watch->is_current(peer));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), DiscoveryWatchError::ClockRegressed);
    EXPECT_FALSE(watch->is_open());
}

TEST_F(NativeDiscoveryWatchTest, SnapshotFailsClosedOnClockRegressionAndClosure) {
    auto result = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, {{4096, 16, 1024}, 4, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(result));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    auto empty = watch->current_peers();
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->empty());
    ASSERT_TRUE(publish(advertisement(1, 100, 150)));
    ASSERT_TRUE(loop.until([&] { return !events.empty(); }));
    auto peers = watch->current_peers();
    ASSERT_TRUE(peers);
    ASSERT_EQ(peers->size(), 1U);
    EXPECT_EQ(peers->front().fields.sequence, 1U);
    time = 99;
    EXPECT_FALSE(watch->current_peers());
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front(), DiscoveryWatchError::ClockRegressed);
    watch->close();
    EXPECT_FALSE(watch->current_peers());
}

TEST_F(NativeDiscoveryWatchTest, LeaseSnapshotRequiresPublishedUnexpiredRecord) {
    std::vector<std::uint64_t> published;
    std::vector<AdvertisementLeaseError> lease_errors;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, lease_config(7),
        MemoryBudget(4096), loop.executor(), [this] { return time; },
        {[&](std::uint64_t seq) { published.push_back(seq); },
         [&](AdvertisementLeaseError error) { lease_errors.push_back(error); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    EXPECT_FALSE(lease->published_fields());
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return !published.empty(); }));
    auto fields = lease->published_fields();
    ASSERT_TRUE(fields);
    EXPECT_EQ(fields->sequence, 7U);
    EXPECT_EQ(fields->issued_at, 100U);
    EXPECT_EQ(fields->expires_at, 120U);
    time = 110;
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(lease->published_fields());
    EXPECT_EQ(lease->published_fields()->sequence, 7U);
    ASSERT_TRUE(loop.until([&] { return published.size() == 2; }));
    fields = lease->published_fields();
    ASSERT_TRUE(fields);
    EXPECT_EQ(fields->sequence, 8U);
    EXPECT_EQ(fields->issued_at, 110U);
    EXPECT_EQ(fields->expires_at, 130U);
    time = 130;
    EXPECT_FALSE(lease->published_fields());
    time = 129;
    EXPECT_FALSE(lease->published_fields());
    ASSERT_EQ(lease_errors.size(), 1U);
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(lease_errors.front()),
              AdvertisementLeaseFailure::ClockRegressed);
    EXPECT_FALSE(lease->is_open());
}

TEST_F(NativeDiscoveryWatchTest, LeaseRefreshesAndExpiresAfterClose) {
    auto result = NativeDiscoveryWatch::create(
        NativeStore(second, {1024, 32, 32768}), capability, {{4096, 16, 1024}, 4, 60, 5},
        MemoryBudget(4096), loop.executor(), [this] { return time; }, callbacks());
    auto watch = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(result));
    ASSERT_EQ(watch->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(watch->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(watch->catch_up()));
    std::vector<std::uint64_t> published;
    std::vector<AdvertisementLeaseError> lease_errors;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, lease_config(7),
        MemoryBudget(4096), loop.executor(), [this] { return time; },
        {[&](std::uint64_t seq) { published.push_back(seq); },
         [&](AdvertisementLeaseError error) { lease_errors.push_back(error); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Pending);
    ASSERT_TRUE(loop.until([&] { return published.size() == 1 && events.size() == 1; }));
    EXPECT_EQ(published.front(), 7U);
    time = 109;
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::NotDue);
    time = 110;
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return published.size() == 2 && events.size() == 2; }));
    EXPECT_EQ(published.back(), 8U);
    EXPECT_EQ(events.back(), AdvertisementEvent::Updated);
    EXPECT_TRUE(lease_errors.empty());
    lease->close();
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Closed);
    time = 135;
    ASSERT_TRUE(watch->tick());
    ASSERT_EQ(expired.size(), 1U);
    EXPECT_EQ(expired.front().fields.sequence, 8U);
}

TEST(NativeAdvertisementLeaseValidation, BoundsRetriesAndRejectsExpiredPendingRecord) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, {1024, 32, 32768});
    auto identity = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    auto capability = std::make_shared<Bytes>(32, std::byte{8});
    std::uint64_t time = 100;
    std::vector<AdvertisementLeaseError> errors;
    auto callbacks = AdvertisementLeaseCallbacks{
        [](std::uint64_t) {}, [&](AdvertisementLeaseError error) { errors.push_back(error); }};
    EXPECT_THROW((NativeAdvertisementLease::create(
                     store, {}, capability, lease_config(), MemoryBudget(4096), [](auto) {},
                     [&] { return time; }, callbacks)),
                 std::invalid_argument);
    EXPECT_THROW((NativeAdvertisementLease::create(
                     store, identity, {}, lease_config(), MemoryBudget(4096), [](auto) {},
                     [&] { return time; }, callbacks)),
                 std::invalid_argument);
    auto created = NativeAdvertisementLease::create(
        store, identity, capability, lease_config(0, 2), MemoryBudget(4096), [](auto) {},
        [&] { return time; }, callbacks);
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<OutboxError>(errors.back()));
    EXPECT_TRUE(lease->retry());
    EXPECT_EQ(errors.size(), 2U);
    EXPECT_FALSE(lease->retry());
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(errors.back()),
              AdvertisementLeaseFailure::PublicationLimit);
    EXPECT_FALSE(lease->is_open());
    errors.clear();
    auto created2 = NativeAdvertisementLease::create(
        store, identity, capability, lease_config(), MemoryBudget(4096), [](auto) {},
        [&] { return time; }, callbacks);
    auto lease2 = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created2));
    EXPECT_EQ(lease2->tick(), AdvertisementLeaseTick::Submitted);
    time = 120;
    EXPECT_FALSE(lease2->retry());
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(errors.back()),
              AdvertisementLeaseFailure::ExpiredPending);
}

TEST_F(NativeDiscoveryWatchTest, LeaseRejectsPublicationCompletionAfterExpiry) {
    int published = 0;
    std::vector<AdvertisementLeaseError> lease_errors;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, lease_config(),
        MemoryBudget(4096), loop.executor(), [this] { return time; },
        {[&](std::uint64_t) { ++published; },
         [&](AdvertisementLeaseError error) { lease_errors.push_back(error); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    time = 120;
    ASSERT_TRUE(loop.until([&] { return !lease_errors.empty(); }));
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(lease_errors.front()),
              AdvertisementLeaseFailure::ExpiredPending);
    EXPECT_EQ(published, 0);
    EXPECT_FALSE(lease->is_open());
}

TEST_F(NativeDiscoveryWatchTest, LeaseDoesNotWrapSequenceNumbers) {
    std::vector<std::uint64_t> published;
    std::vector<AdvertisementLeaseError> lease_errors;
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability,
        lease_config(std::numeric_limits<std::uint64_t>::max()), MemoryBudget(4096),
        loop.executor(), [this] { return time; },
        {[&](std::uint64_t value) { published.push_back(value); },
         [&](AdvertisementLeaseError error) { lease_errors.push_back(error); }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    ASSERT_TRUE(loop.until([&] { return !published.empty() || !lease_errors.empty(); }));
    ASSERT_TRUE(lease_errors.empty());
    time = 110;
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Failed);
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(lease_errors.front()),
              AdvertisementLeaseFailure::SequenceExhausted);
}

TEST_F(NativeDiscoveryWatchTest, ClosingLeaseSuppressesQueuedCompletion) {
    int callbacks = 0;
    MemoryBudget budget(4096);
    auto created = NativeAdvertisementLease::create(
        NativeStore(first, {1024, 32, 32768}), identity, capability, lease_config(), budget,
        loop.executor(), [this] { return time; },
        {[&](std::uint64_t) { ++callbacks; }, [&](AdvertisementLeaseError) { ++callbacks; }});
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!loop.has_tasks() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(loop.has_tasks());
    lease->close();
    ASSERT_TRUE(loop.until([] { return true; }));
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(NativeAdvertisementLeaseValidation, RejectsClockRollbackAndTimeOverflow) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, {1024, 32, 32768});
    auto identity = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    auto capability = std::make_shared<Bytes>(32, std::byte{8});
    std::uint64_t time = 100;
    std::vector<AdvertisementLeaseError> errors;
    auto callbacks = AdvertisementLeaseCallbacks{
        [](std::uint64_t) {}, [&](AdvertisementLeaseError error) { errors.push_back(error); }};
    auto created = NativeAdvertisementLease::create(
        store, identity, capability, lease_config(), MemoryBudget(4096), [](auto) {},
        [&] { return time; }, callbacks);
    auto lease = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created));
    ASSERT_EQ(lease->tick(), AdvertisementLeaseTick::Submitted);
    time = 99;
    EXPECT_EQ(lease->tick(), AdvertisementLeaseTick::Failed);
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(errors.back()),
              AdvertisementLeaseFailure::ClockRegressed);
    errors.clear();
    time = std::numeric_limits<std::uint64_t>::max() - 10;
    auto created2 = NativeAdvertisementLease::create(
        store, identity, capability, lease_config(), MemoryBudget(4096), [](auto) {},
        [&] { return time; }, callbacks);
    auto lease2 = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(created2));
    EXPECT_EQ(lease2->tick(), AdvertisementLeaseTick::Failed);
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(errors.back()),
              AdvertisementLeaseFailure::TimeOverflow);
}
