#include "../src/core/deferred_events.hpp"
#include "../src/discovery/native_discovery_cycle.hpp"
#include "../src/discovery/native_runner_owner.hpp"
#include "native_discovery_watch_fixture.hpp"

namespace {
class ManualScheduler {
  public:
    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            callbacks_.push_back(std::move(callback));
            return [] {};
        };
    }
    void fire_next() {
        if (next_ < callbacks_.size())
            callbacks_[next_++]();
    }
    std::function<void()> last() const { return callbacks_.back(); }
    std::size_t size() const { return callbacks_.size(); }

  private:
    std::vector<std::function<void()>> callbacks_;
    std::size_t next_ = 0;
};

class NativeDiscoveryCycleTest : public NativeDiscoveryWatchTest {
  protected:
    ManualScheduler scheduler;
    unsigned published = 0;
    std::vector<AdvertisementLeaseError> lease_errors;
    std::vector<DiscoveryCycleError> cycle_errors;

    DiscoveryCycleConfig config() {
        return {NativeStore(second, {1024, 32, 32768}),
                NativeStore(first, {1024, 32, 32768}),
                identity,
                capability,
                DiscoveryWatchLimits{{4096, 16, 1024}, 4, 60, 5},
                lease_config(),
                MemoryBudget(8192),
                10ms,
                2,
                [this] { return time; }};
    }
    DiscoveryCycleCallbacks handlers() {
        return {callbacks(),
                {[this](std::uint64_t) { ++published; },
                 [this](AdvertisementLeaseError error) { lease_errors.push_back(error); }},
                [this](DiscoveryCycleError error) { cycle_errors.push_back(error); }};
    }
    std::unique_ptr<NativeDiscoveryCycle> create() {
        auto result = NativeDiscoveryCycle::create(config(), loop.executor(), scheduler.scheduler(),
                                                   handlers());
        return std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    }
    bool advance_until(const std::function<bool()> &condition) {
        return loop.until([&] {
            if (condition())
                return true;
            scheduler.fire_next();
            return false;
        });
    }
    bool pulse_once(const NativeDiscoveryCycle &cycle) {
        const auto previous = scheduler.size();
        scheduler.fire_next();
        return loop.until([&] { return scheduler.size() > previous || !cycle.is_open(); });
    }
};
} // namespace

TEST_F(NativeDiscoveryCycleTest, AutomaticallyDiscoversRefreshesAndExpires) {
    auto cycle = create();
    ASSERT_TRUE(cycle->start());
    EXPECT_FALSE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && published == 1 && !events.empty(); }));
    ASSERT_TRUE(*caught_up);
    ASSERT_EQ(events.front(), AdvertisementEvent::Seen);
    time = 110;
    ASSERT_TRUE(advance_until([&] { return published == 2 && events.size() == 2; }));
    EXPECT_EQ(events.back(), AdvertisementEvent::Updated);
    cycle->stop_announcing();
    time = 135;
    ASSERT_TRUE(advance_until([&] { return expired.size() == 1; }));
    EXPECT_EQ(expired.front().fields.sequence, 1U);
    EXPECT_TRUE(cycle_errors.empty());
    EXPECT_TRUE(lease_errors.empty());
    auto late = scheduler.last();
    cycle->close();
    const auto submitted = loop.submitted();
    late();
    EXPECT_EQ(loop.submitted(), submitted);
    EXPECT_FALSE(cycle->is_open());
    EXPECT_TRUE(loop.until([] { return true; }));
    EXPECT_EQ(expired.size(), 1U);
}

TEST_F(NativeDiscoveryCycleTest, WindowedCycleAutomaticallyTracksAcrossRollover) {
    time = 110;
    auto options = config();
    options.watch_limits->windowed = true;
    options.lease_config->windowed = true;
    auto result =
        NativeDiscoveryCycle::create(options, loop.executor(), scheduler.scheduler(), handlers());
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && published == 1 && events.size() == 1; }));
    EXPECT_TRUE(*caught_up);
    EXPECT_EQ(events.front(), AdvertisementEvent::Seen);
    time = 120;
    caught_up.reset();
    ASSERT_TRUE(advance_until([&] { return caught_up && published == 2 && events.size() == 2; }));
    EXPECT_TRUE(*caught_up);
    EXPECT_EQ(events.back(), AdvertisementEvent::Updated);
    EXPECT_TRUE(cycle_errors.empty());
    EXPECT_TRUE(lease_errors.empty());
}

TEST_F(NativeDiscoveryCycleTest, OwnedRunnerDiscoversThenJoinsAfterCycleCloses) {
    std::shared_ptr<dht::DhtRunner> owned;
    auto address = first->getBound();
    auto owner = NativeRunnerOwner::create(
        loop.executor(),
        [address](dht::DhtRunner &runner) {
            dht::DhtRunner::Config options;
            options.dht_config.node_config.network = 0x12345684;
            options.peer_discovery = false;
            options.peer_publish = false;
            runner.run("127.0.0.1", "::1", "0", options);
            auto done = std::make_shared<std::promise<bool>>();
            auto result = done->get_future();
            runner.bootstrap(address, [done](bool success) { done->set_value(success); });
            if (result.wait_for(10s) != std::future_status::ready || !result.get()) {
                throw std::runtime_error("local bootstrap failed");
            }
        },
        [&](RunnerStartResult result) {
            owned = std::get<std::shared_ptr<dht::DhtRunner>>(std::move(result));
        });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(loop.until([&] { return static_cast<bool>(owned); }));
    auto options = config();
    options.watch_store = NativeStore(owned, {1024, 32, 32768});
    auto result =
        NativeDiscoveryCycle::create(options, loop.executor(), scheduler.scheduler(), handlers());
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && published == 1 && !events.empty(); }));
    EXPECT_TRUE(*caught_up);
    EXPECT_EQ(events.front(), AdvertisementEvent::Seen);
    cycle->close();
    cycle.reset();
    bool stopped = false;
    ASSERT_TRUE(owner->shutdown([&](std::exception_ptr error) {
        EXPECT_FALSE(error);
        EXPECT_FALSE(owned->isRunning());
        stopped = true;
    }));
    ASSERT_TRUE(loop.until([&] { return stopped; }));
}

TEST_F(NativeDiscoveryCycleTest, CloseSuppressesAQueuedTimerAndLateFirings) {
    auto cycle = create();
    ASSERT_TRUE(cycle->start());
    scheduler.fire_next();
    auto late = scheduler.last();
    cycle->close();
    const auto submitted = loop.submitted();
    late();
    EXPECT_EQ(loop.submitted(), submitted);
    EXPECT_TRUE(loop.until([] { return true; }));
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(published, 0U);
    EXPECT_TRUE(cycle_errors.empty());
}

TEST_F(NativeDiscoveryCycleTest, PublicationLimitStopsTheCycle) {
    auto options = config();
    options.lease_config->max_publications = 1;
    auto result =
        NativeDiscoveryCycle::create(options, loop.executor(), scheduler.scheduler(), handlers());
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && *caught_up && published == 1; }));
    time = 110;
    ASSERT_TRUE(advance_until([&] { return !cycle_errors.empty(); }));
    ASSERT_EQ(cycle_errors.size(), 1U);
    EXPECT_EQ(cycle_errors.front(), DiscoveryCycleError::Lease);
    ASSERT_FALSE(lease_errors.empty());
    EXPECT_EQ(std::get<AdvertisementLeaseFailure>(lease_errors.back()),
              AdvertisementLeaseFailure::PublicationLimit);
    EXPECT_FALSE(cycle->is_open());
}

TEST_F(NativeDiscoveryCycleTest, ClockRegressionStopsTheCycle) {
    auto cycle = create();
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && *caught_up && published == 1; }));
    time = 99;
    ASSERT_TRUE(advance_until([&] { return !cycle_errors.empty(); }));
    ASSERT_EQ(cycle_errors.size(), 1U);
    EXPECT_EQ(cycle_errors.front(), DiscoveryCycleError::Watch);
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors.back(), DiscoveryWatchError::ClockRegressed);
    EXPECT_FALSE(cycle->is_open());
}

TEST_F(NativeDiscoveryCycleTest, StoppedWatchBackendReportsIncompleteDiscovery) {
    auto cycle = create();
    second->join();
    (void)cycle->start();
    ASSERT_TRUE(advance_until([&] { return !cycle_errors.empty(); }));
    ASSERT_EQ(cycle_errors.size(), 1U);
    EXPECT_TRUE(cycle_errors.front() == DiscoveryCycleError::Registration ||
                cycle_errors.front() == DiscoveryCycleError::Catchup);
    EXPECT_FALSE(cycle->is_open());
    const auto published_at_failure = published;
    EXPECT_TRUE(loop.until([] { return true; }));
    EXPECT_EQ(published, published_at_failure);
}

TEST_F(NativeDiscoveryCycleTest, SchedulerFailureClosesChildrenAndReportsError) {
    Scheduler broken = [](auto, auto) -> CancelTimer { throw std::runtime_error("unavailable"); };
    auto result = NativeDiscoveryCycle::create(config(), loop.executor(), broken, handlers());
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    EXPECT_FALSE(cycle->start());
    EXPECT_FALSE(cycle->is_open());
    ASSERT_EQ(cycle_errors.size(), 1U);
    EXPECT_EQ(cycle_errors.front(), DiscoveryCycleError::Scheduler);
    EXPECT_TRUE(loop.until([] { return true; }));
    EXPECT_EQ(published, 0U);
}

TEST_F(NativeDiscoveryCycleTest, RetriesOnlyAfterConfiguredPulseDelay) {
    auto cycle = create();
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && *caught_up && published == 1; }));
    first->join();
    time = 110;
    ASSERT_TRUE(advance_until([&] { return lease_errors.size() == 1; }));
    ASSERT_TRUE(pulse_once(*cycle));
    EXPECT_EQ(lease_errors.size(), 1U);
    ASSERT_TRUE(pulse_once(*cycle));
    EXPECT_EQ(lease_errors.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<OutboxError>(lease_errors.back()));
    EXPECT_TRUE(cycle_errors.empty());
    cycle->close();
}

TEST_F(NativeDiscoveryCycleTest, RetryDelayDoublesAfterConsecutiveFailures) {
    auto options = config();
    options.lease_config->max_publications = 8;
    auto result =
        NativeDiscoveryCycle::create(options, loop.executor(), scheduler.scheduler(), handlers());
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return caught_up && *caught_up && published == 1; }));
    first->join();
    time = 110;
    ASSERT_TRUE(advance_until([&] { return lease_errors.size() == 1; }));
    const auto pulses_until_next_error = [&] {
        const auto before = lease_errors.size();
        unsigned pulses = 0;
        while (lease_errors.size() == before && pulses < 64) {
            if (!pulse_once(*cycle))
                return 0U;
            ++pulses;
        }
        return pulses;
    };
    EXPECT_EQ(pulses_until_next_error(), 2U);
    EXPECT_EQ(pulses_until_next_error(), 4U);
    EXPECT_EQ(pulses_until_next_error(), 8U);
    EXPECT_TRUE(cycle_errors.empty());
    cycle->close();
}

TEST_F(NativeDiscoveryCycleTest, RejectsInvalidRuntimeBeforeStarting) {
    auto invalid = config();
    invalid.period = 0ms;
    EXPECT_THROW(
        (NativeDiscoveryCycle::create(invalid, loop.executor(), scheduler.scheduler(), handlers())),
        std::invalid_argument);
    EXPECT_THROW((NativeDiscoveryCycle::create(config(), {}, scheduler.scheduler(), handlers())),
                 std::invalid_argument);
    invalid = config();
    invalid.watch_limits.reset();
    invalid.lease_config.reset();
    EXPECT_THROW(
        (NativeDiscoveryCycle::create(invalid, loop.executor(), scheduler.scheduler(), handlers())),
        std::invalid_argument);
    invalid = config();
    invalid.retry_after_pulses = 0;
    EXPECT_THROW(
        (NativeDiscoveryCycle::create(invalid, loop.executor(), scheduler.scheduler(), handlers())),
        std::invalid_argument);
    auto missing = handlers();
    missing.watch.on_catchup = {};
    EXPECT_THROW(
        (NativeDiscoveryCycle::create(config(), loop.executor(), scheduler.scheduler(), missing)),
        std::invalid_argument);
    missing = handlers();
    missing.lease.on_error = {};
    EXPECT_THROW(
        (NativeDiscoveryCycle::create(config(), loop.executor(), scheduler.scheduler(), missing)),
        std::invalid_argument);
}

TEST_F(NativeDiscoveryCycleTest, SupportsWatchingWithoutAnAnnouncement) {
    auto options = config();
    options.lease_config.reset();
    options.identity.reset();
    auto callbacks = handlers();
    callbacks.lease = {};
    auto result =
        NativeDiscoveryCycle::create(options, loop.executor(), scheduler.scheduler(), callbacks);
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(publish(advertisement(1, 100, 150)));
    ASSERT_TRUE(advance_until([&] { return caught_up && *caught_up && !events.empty(); }));
    EXPECT_EQ(events.front(), AdvertisementEvent::Seen);
    EXPECT_EQ(published, 0U);
    cycle->stop_watching();
    EXPECT_FALSE(cycle->is_open());
}

TEST_F(NativeDiscoveryCycleTest, SupportsAnnouncementWithoutAWatch) {
    auto options = config();
    options.watch_limits.reset();
    auto callbacks = handlers();
    callbacks.watch = {};
    auto result =
        NativeDiscoveryCycle::create(options, loop.executor(), scheduler.scheduler(), callbacks);
    auto cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return published == 1; }));
    EXPECT_TRUE(events.empty());
    cycle->stop_announcing();
    EXPECT_FALSE(cycle->is_open());
}

TEST_F(NativeDiscoveryCycleTest, DeferredPeerCallbackCanStopItsWatch) {
    auto queue = std::make_shared<DeferredEvents>(loop.executor(), MemoryBudget(1024), 4, 1024);
    std::unique_ptr<NativeDiscoveryCycle> cycle;
    int delivered = 0;
    auto callbacks = handlers();
    callbacks.watch.on_peer = [&](Advertisement peer, AdvertisementEvent event) {
        EXPECT_EQ(queue->post(sizeof(peer),
                              [&, peer, event] {
                                  EXPECT_EQ(event, AdvertisementEvent::Seen);
                                  EXPECT_EQ(peer.fields.sequence, 0U);
                                  ++delivered;
                                  cycle->stop_watching();
                                  queue->close();
                              }),
                  DeferredEventResult::Accepted);
    };
    auto result =
        NativeDiscoveryCycle::create(config(), loop.executor(), scheduler.scheduler(), callbacks);
    cycle = std::move(std::get<std::unique_ptr<NativeDiscoveryCycle>>(result));
    ASSERT_TRUE(cycle->start());
    ASSERT_TRUE(advance_until([&] { return delivered == 1 && published == 1; }));
    EXPECT_TRUE(cycle->is_open());
    time = 110;
    ASSERT_TRUE(advance_until([&] { return published == 2; }));
    EXPECT_EQ(delivered, 1);
    cycle->close();
    queue->close();
}
