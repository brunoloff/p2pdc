#include "../src/discovery/native_public_watch.hpp"
#include "../src/discovery/native_windowed_mailbox.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
struct FakeWatchState {
    std::promise<std::size_t> promise;
    std::shared_future<std::size_t> ready = promise.get_future().share();
    std::function<void(StoreBatch)> callback;
    bool canceled = false;
};
class FakeWatch final : public RecordWatchBackend {
  public:
    explicit FakeWatch(std::shared_ptr<FakeWatchState> state) : state_(std::move(state)) {}
    ~FakeWatch() override { cancel(); }
    void cancel() override { state_->canceled = true; }
    std::shared_future<std::size_t> registration() const override { return state_->ready; }

  private:
    std::shared_ptr<FakeWatchState> state_;
};
class FakeStore final : public RecordStoreBackend {
  public:
    mutable std::vector<std::shared_ptr<FakeWatchState>> watches;
    mutable std::optional<StoreError> next_watch_error;
    mutable std::vector<std::function<void(LookupResult)>> pending_lookups;
    bool hold_lookups = false;
    std::vector<Bytes> values;
    std::optional<StoreError> put(ByteView, ByteView, std::function<void(bool)>) const override {
        return StoreError::NotRunning;
    }
    std::optional<StoreError> lookup(ByteView, std::function<void(LookupResult)> done,
                                     LookupDeduplication) const override {
        if (hold_lookups)
            pending_lookups.push_back(std::move(done));
        else
            done({StoreBatch{values, {}, false}, true});
        return std::nullopt;
    }
    void finish_lookup() {
        auto done = std::move(pending_lookups.front());
        pending_lookups.erase(pending_lookups.begin());
        done({StoreBatch{values, {}, false}, true});
    }
    std::variant<RecordWatch, StoreError> watch(ByteView, std::function<void(StoreBatch)> callback,
                                                bool) const override {
        if (next_watch_error) {
            auto error = *next_watch_error;
            next_watch_error.reset();
            return error;
        }
        auto state = std::make_shared<FakeWatchState>();
        state->callback = std::move(callback);
        state->promise.set_value(1);
        watches.push_back(state);
        return RecordWatch(std::make_unique<FakeWatch>(state));
    }
};
class DiscoveryRecoveryTest : public ::testing::Test {
  protected:
    struct Timer {
        std::function<void()> task;
        bool canceled = false;
    };
    std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
    std::deque<std::function<void()>> tasks;
    std::deque<std::shared_ptr<Timer>> timers;
    std::vector<DiscoveryStatus> statuses;
    std::vector<WatchError> errors;
    std::vector<Advertisement> peers;
    unsigned catchups = 0;
    bool close_on_status = false;
    std::shared_ptr<DiscoveryBackend> watch;
    DiscoveryCapability capability{};
    Identity identity = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    RecipientKeyPair box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{2})));
    Executor executor() {
        return [this](auto task) { tasks.push_back(std::move(task)); };
    }
    Scheduler scheduler() {
        return [this](auto, auto task) -> CancelTimer {
            auto timer = std::make_shared<Timer>();
            timer->task = std::move(task);
            timers.push_back(timer);
            return [timer] { timer->canceled = true; };
        };
    }
    bool advance(const std::function<bool()> &condition);
    Bytes advertisement(std::uint64_t sequence);
    void begin(DiscoveryProtocol protocol, std::size_t retries = 2);
    void interrupt(StoreError error);
};
bool DiscoveryRecoveryTest::advance(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
        if (condition())
            return true;
        if (!timers.empty()) {
            auto timer = std::move(timers.front());
            timers.pop_front();
            if (!timer->canceled)
                timer->task();
        }
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}
Bytes DiscoveryRecoveryTest::advertisement(std::uint64_t sequence) {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto now = static_cast<std::uint64_t>(value);
    AdvertisementFields fields{box.public_key(), {std::byte{3}}, {std::byte{4}}, sequence, 0, now,
                               now + 30};
    return std::get<Bytes>(encode_advertisement(identity, fields, capability, {now, 60, 5}));
}
void DiscoveryRecoveryTest::begin(DiscoveryProtocol protocol, std::size_t retries) {
    capability.fill(std::byte{9});
    store->values = {advertisement(1)};
    WatchOptions options;
    options.protocol = protocol;
    options.max_advertisement_lifetime = 300;
    options.clock_skew = 5;
    options.pulse_interval = 10ms;
    options.max_recovery_retries = retries;
    options.recovery_delay = 10ms;
    DiscoveryHandlers handlers{
        [this](Advertisement value, PeerEvent) { peers.push_back(std::move(value)); },
        [this](bool ok) {
            if (ok)
                ++catchups;
        },
        [this](WatchError error) { errors.push_back(error); },
        [this](DiscoveryStatus value) {
            statuses.push_back(value);
            if (close_on_status && watch)
                watch->close();
        }};
    auto result =
        make_public_watch(RecordStore(store), capability, scheduler(), options, std::move(handlers),
                          executor(), CallbackGate(executor()), MemoryBudget(4 * 1024 * 1024));
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<DiscoveryBackend>>(result));
    watch = std::get<std::shared_ptr<DiscoveryBackend>>(std::move(result));
    ASSERT_TRUE(advance([&] { return catchups == 1 && !peers.empty(); }));
}
void DiscoveryRecoveryTest::interrupt(StoreError error) {
    ASSERT_FALSE(store->watches.empty());
    store->watches.front()->callback(StoreBatch{{}, error, false});
    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
}
} // namespace

TEST_F(DiscoveryRecoveryTest, FixedKeyWatchRecoversAndCatchesUp) {
    ASSERT_NO_FATAL_FAILURE(begin(DiscoveryProtocol::FixedKeyV1));
    ASSERT_NO_FATAL_FAILURE(interrupt(StoreError::NotRunning));
    EXPECT_FALSE(watch->healthy());
    EXPECT_TRUE(watch->recovering());
    EXPECT_FALSE(watch->current_peers());
    store->values = {advertisement(2)};
    ASSERT_TRUE(
        advance([&] { return statuses.size() == 2 && catchups == 2 && peers.size() == 2; }));
    EXPECT_EQ(statuses, (std::vector<DiscoveryStatus>{DiscoveryStatus::Reconnecting,
                                                      DiscoveryStatus::Ready}));
    EXPECT_TRUE(watch->healthy());
    EXPECT_FALSE(watch->recovering());
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(peers.back().fields.sequence, 2U);
    watch->close();
}
TEST_F(DiscoveryRecoveryTest, RotatingWatchRecoversAndCatchesUp) {
    ASSERT_NO_FATAL_FAILURE(begin(DiscoveryProtocol::RotatingKeysV2));
    ASSERT_GT(store->watches.size(), 1U);
    ASSERT_NO_FATAL_FAILURE(interrupt(StoreError::NotRunning));
    EXPECT_FALSE(watch->healthy());
    EXPECT_TRUE(watch->recovering());
    store->values = {advertisement(2)};
    ASSERT_TRUE(
        advance([&] { return statuses.size() == 2 && catchups == 2 && peers.size() == 2; }));
    EXPECT_EQ(statuses, (std::vector<DiscoveryStatus>{DiscoveryStatus::Reconnecting,
                                                      DiscoveryStatus::Ready}));
    EXPECT_TRUE(watch->healthy());
    EXPECT_FALSE(watch->recovering());
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(peers.back().fields.sequence, 2U);
    watch->close();
}
TEST_F(DiscoveryRecoveryTest, InvalidRecordIsTerminalRatherThanRetried) {
    ASSERT_NO_FATAL_FAILURE(begin(DiscoveryProtocol::FixedKeyV1));
    ASSERT_NO_FATAL_FAILURE(interrupt(StoreError::InvalidRecord));
    EXPECT_FALSE(watch->healthy());
    EXPECT_FALSE(watch->recovering());
    ASSERT_TRUE(advance([&] { return !errors.empty(); }));
    EXPECT_EQ(errors.front(), WatchError::Backend);
    EXPECT_TRUE(statuses.empty());
    EXPECT_EQ(store->watches.size(), 1U);
    watch->close();
}

TEST_F(DiscoveryRecoveryTest, ExhaustedRetriesReportTerminalFailure) {
    ASSERT_NO_FATAL_FAILURE(begin(DiscoveryProtocol::FixedKeyV1, 1));
    store->next_watch_error = StoreError::NotRunning;
    ASSERT_NO_FATAL_FAILURE(interrupt(StoreError::NotRunning));
    ASSERT_TRUE(advance([&] { return !errors.empty(); }));
    EXPECT_EQ(errors.front(), WatchError::Backend);
    EXPECT_EQ(statuses, (std::vector<DiscoveryStatus>{DiscoveryStatus::Reconnecting}));
    EXPECT_FALSE(watch->healthy());
    EXPECT_FALSE(watch->recovering());
    watch->close();
}
TEST_F(DiscoveryRecoveryTest, StatusCallbackCanCloseDuringRecovery) {
    ASSERT_NO_FATAL_FAILURE(begin(DiscoveryProtocol::FixedKeyV1));
    close_on_status = true;
    ASSERT_NO_FATAL_FAILURE(interrupt(StoreError::NotRunning));
    ASSERT_TRUE(advance([&] { return !statuses.empty(); }));
    EXPECT_EQ(statuses.front(), DiscoveryStatus::Reconnecting);
    EXPECT_FALSE(watch->healthy());
    EXPECT_FALSE(watch->recovering());
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(store->watches.front()->canceled);
}
TEST_F(DiscoveryRecoveryTest, RejectsInvalidRecoveryOptions) {
    DiscoveryHandlers handlers{[](Advertisement, PeerEvent) {}, [](bool) {}, [](WatchError) {}};
    WatchOptions options;
    options.max_recovery_retries = 11;
    EXPECT_THROW(validate_watch(scheduler(), options, handlers), std::invalid_argument);
    options.max_recovery_retries = 1;
    options.recovery_delay = 9ms;
    EXPECT_THROW(validate_watch(scheduler(), options, handlers), std::invalid_argument);
    options.recovery_delay = 10ms;
    EXPECT_NO_THROW(validate_watch(scheduler(), options, handlers));
}

TEST_F(DiscoveryRecoveryTest, RotatingWatchWaitsForBothInterruptedWindows) {
    ASSERT_NO_FATAL_FAILURE(begin(DiscoveryProtocol::RotatingKeysV2));
    ASSERT_GT(store->watches.size(), 1U);
    store->hold_lookups = true;
    store->watches[0]->callback(StoreBatch{{}, StoreError::NotRunning, false});
    store->watches[1]->callback(StoreBatch{{}, StoreError::NotRunning, false});
    ASSERT_TRUE(advance([&] { return store->pending_lookups.size() == 2; }));
    EXPECT_EQ(statuses, (std::vector<DiscoveryStatus>{DiscoveryStatus::Reconnecting}));
    store->finish_lookup();
    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
    EXPECT_FALSE(watch->healthy());
    EXPECT_TRUE(watch->recovering());
    EXPECT_EQ(statuses.size(), 1U);
    store->finish_lookup();
    ASSERT_TRUE(advance([&] { return statuses.size() == 2; }));
    EXPECT_EQ(statuses.back(), DiscoveryStatus::Ready);
    EXPECT_TRUE(watch->healthy());
    watch->close();
}

TEST_F(DiscoveryRecoveryTest, RetiredRecoveringWindowWaitsForReplacementCatchup) {
    capability.fill(std::byte{9});
    std::vector<RecoveryState> observed;
    unsigned completed = 0;
    auto secret = std::make_shared<const Bytes>(capability.begin(), capability.end());
    NativeWindowedMailbox mailbox(
        RecordStore(store), secret, {4096, 16, 1024}, 60, 5, MemoryBudget(8192), executor(),
        {[](Bytes) {}, [](auto, auto) { ADD_FAILURE(); }, [&](bool ok) { completed += ok; },
         [&](RecoveryState value) { observed.push_back(value); }},
        RecoveryOptions{2, 10ms, 100ms});
    ASSERT_TRUE(mailbox.tick(120));
    ASSERT_TRUE(advance([&] { return completed == 1; }));
    ASSERT_EQ(store->watches.size(), 2U);
    store->watches[0]->callback(StoreBatch{{}, StoreError::NotRunning, false});
    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
    ASSERT_TRUE(mailbox.tick(120));
    ASSERT_EQ(observed, (std::vector<RecoveryState>{RecoveryState::Reconnecting}));
    store->hold_lookups = true;
    ASSERT_TRUE(mailbox.tick(235));
    ASSERT_EQ(store->pending_lookups.size(), 1U);
    EXPECT_FALSE(mailbox.available());
    EXPECT_TRUE(mailbox.recovering());
    EXPECT_EQ(observed.size(), 1U);
    store->finish_lookup();
    ASSERT_TRUE(advance([&] { return observed.size() == 2; }));
    EXPECT_EQ(observed.back(), RecoveryState::Ready);
    EXPECT_TRUE(mailbox.available());
    EXPECT_FALSE(mailbox.recovering());
    mailbox.close();
}
