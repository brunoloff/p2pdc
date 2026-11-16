#include "../src/discovery/recovering_mailbox.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <future>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
struct WatchState {
    std::promise<std::size_t> promise;
    std::shared_future<std::size_t> ready = promise.get_future().share();
    std::function<void(StoreBatch)> callback;
    bool canceled = false;
};
class Watch final : public RecordWatchBackend {
  public:
    explicit Watch(std::shared_ptr<WatchState> state) : state_(std::move(state)) {}
    ~Watch() override { cancel(); }
    void cancel() override { state_->canceled = true; }
    std::shared_future<std::size_t> registration() const override { return state_->ready; }

  private:
    std::shared_ptr<WatchState> state_;
};
class Store final : public RecordStoreBackend {
  public:
    mutable std::vector<std::shared_ptr<WatchState>> watches;
    mutable std::vector<std::function<void(LookupResult)>> lookups;
    mutable std::optional<StoreError> next_watch_error;
    mutable std::optional<StoreError> next_lookup_error;
    std::optional<StoreError> put(ByteView, ByteView, std::function<void(bool)>) const override {
        return StoreError::NotRunning;
    }
    std::optional<StoreError> lookup(ByteView, std::function<void(LookupResult)> done,
                                     LookupDeduplication) const override {
        if (next_lookup_error) {
            auto error = *next_lookup_error;
            next_lookup_error.reset();
            return error;
        }
        lookups.push_back(std::move(done));
        return std::nullopt;
    }
    std::variant<RecordWatch, StoreError> watch(ByteView, std::function<void(StoreBatch)> callback,
                                                bool) const override {
        if (next_watch_error) {
            auto error = *next_watch_error;
            next_watch_error.reset();
            return error;
        }
        auto state = std::make_shared<WatchState>();
        state->callback = std::move(callback);
        watches.push_back(state);
        return RecordWatch(std::make_unique<Watch>(std::move(state)));
    }
};
class RecoveringMailboxTest : public ::testing::Test {
  protected:
    using Clock = RecoveringMailbox::Clock;
    std::shared_ptr<Store> backend = std::make_shared<Store>();
    RecordStore store{backend};
    std::deque<std::function<void()>> tasks;
    std::vector<Bytes> records;
    std::vector<RecoveryState> statuses;
    std::vector<RecoveryError> errors;
    int catchups = 0;
    std::unique_ptr<RecoveringMailbox> mailbox;
    const std::array<std::byte, 20> key{std::byte{1}};
    Clock::time_point now{};
    Executor executor() {
        return [this](auto task) { tasks.push_back(std::move(task)); };
    }
    RecoveryCallbacks callbacks() {
        return {[this](Bytes value) { records.push_back(std::move(value)); },
                [this] { ++catchups; }, [this](RecoveryState value) { statuses.push_back(value); },
                [this](RecoveryError error) { errors.push_back(error); }};
    }
    void create(RecoveryOptions options = {}) {
        mailbox = std::make_unique<RecoveringMailbox>(store, key, MailboxLimits{4096, 16, 1024},
                                                      MemoryBudget(8192), executor(), options,
                                                      callbacks());
    }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
    void open_and_catch_up(std::size_t index, Bytes value = {}) {
        ASSERT_TRUE(mailbox->poll(now));
        ASSERT_GT(backend->watches.size(), index);
        backend->watches[index]->promise.set_value(1);
        ASSERT_TRUE(mailbox->poll(now));
        ASSERT_GT(backend->lookups.size(), index);
        StoreBatch batch;
        if (!value.empty())
            batch.values.push_back(std::move(value));
        backend->lookups[index]({std::move(batch), true});
        drain();
        ASSERT_EQ(mailbox->state(), RecoveryState::Ready);
    }
    void fail_watch(std::size_t index, StoreError error) {
        backend->watches[index]->callback(StoreBatch{{}, error, false});
        drain();
    }
};
} // namespace

TEST_F(RecoveringMailboxTest, ReopensAndCatchesUpAfterTransientFailure) {
    create();
    ASSERT_NO_FATAL_FAILURE(open_and_catch_up(0, Bytes{std::byte{1}}));
    EXPECT_EQ(records, (std::vector<Bytes>{Bytes{std::byte{1}}}));
    fail_watch(0, StoreError::NotRunning);
    EXPECT_EQ(mailbox->state(), RecoveryState::Reconnecting);
    ASSERT_TRUE(mailbox->poll(now));
    ASSERT_TRUE(backend->watches[0]->canceled);
    EXPECT_EQ(statuses,
              (std::vector<RecoveryState>{RecoveryState::Ready, RecoveryState::Reconnecting}));
    now += 999ms;
    ASSERT_TRUE(mailbox->poll(now));
    EXPECT_EQ(backend->watches.size(), 1U);
    now += 1ms;
    ASSERT_NO_FATAL_FAILURE(open_and_catch_up(1, Bytes{std::byte{2}}));
    EXPECT_EQ(records, (std::vector<Bytes>{Bytes{std::byte{1}}, Bytes{std::byte{2}}}));
    EXPECT_EQ(catchups, 2);
    EXPECT_EQ(statuses.back(), RecoveryState::Ready);
    EXPECT_TRUE(errors.empty());
}

TEST_F(RecoveringMailboxTest, ExhaustsRetriesAndIgnoresLateCallbacksAfterClose) {
    create({1, 10ms, 100ms});
    ASSERT_NO_FATAL_FAILURE(open_and_catch_up(0));
    auto old = backend->watches[0];
    fail_watch(0, StoreError::NotRunning);
    ASSERT_TRUE(mailbox->poll(now));
    now += 10ms;
    ASSERT_TRUE(mailbox->poll(now));
    ASSERT_EQ(backend->watches.size(), 2U);
    backend->watches[1]->promise.set_value(0);
    EXPECT_FALSE(mailbox->poll(now));
    EXPECT_EQ(mailbox->state(), RecoveryState::Failed);
    ASSERT_EQ(errors, (std::vector<RecoveryError>{RecoveryError::Backend}));
    mailbox->close();
    old->callback(StoreBatch{{Bytes{std::byte{9}}}, {}, false});
    drain();
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(mailbox->state(), RecoveryState::Closed);
}

TEST_F(RecoveringMailboxTest, InvalidRecordsAndClockRegressionFailWithoutRetry) {
    create();
    ASSERT_NO_FATAL_FAILURE(open_and_catch_up(0));
    fail_watch(0, StoreError::InvalidRecord);
    EXPECT_EQ(mailbox->state(), RecoveryState::Failed);
    EXPECT_TRUE(mailbox->is_open());
    EXPECT_FALSE(mailbox->poll(now));
    EXPECT_FALSE(mailbox->is_open());
    EXPECT_EQ(errors, (std::vector<RecoveryError>{RecoveryError::Backend}));
    create();
    ASSERT_TRUE(mailbox->poll(now));
    EXPECT_FALSE(mailbox->poll(now - 1ms));
    EXPECT_EQ(errors.back(), RecoveryError::Clock);
}

TEST_F(RecoveringMailboxTest, RetriesStartupAndRejectsInvalidConfiguration) {
    create({1, 10ms, 100ms});
    backend->next_watch_error = StoreError::NotRunning;
    ASSERT_TRUE(mailbox->poll(now));
    ASSERT_EQ(mailbox->state(), RecoveryState::Reconnecting);
    now += 10ms;
    ASSERT_NO_FATAL_FAILURE(open_and_catch_up(0));
    EXPECT_EQ(statuses,
              (std::vector<RecoveryState>{RecoveryState::Reconnecting, RecoveryState::Ready}));
    EXPECT_THROW((RecoveringMailbox(store, {}, {4096, 16, 1024}, MemoryBudget(8192), executor(), {},
                                    callbacks())),
                 std::invalid_argument);
    EXPECT_THROW(
        (RecoveringMailbox(store, key, {1, 0, 2}, MemoryBudget(8192), executor(), {}, callbacks())),
        std::invalid_argument);
    EXPECT_THROW((RecoveringMailbox(store, key, {4096, 16, 1024}, MemoryBudget(8192), executor(),
                                    {11, 10ms, 100ms}, callbacks())),
                 std::invalid_argument);
}

TEST_F(RecoveringMailboxTest, RetriesAnImmediateTransientCatchupError) {
    create({1, 10ms, 100ms});
    ASSERT_TRUE(mailbox->poll(now));
    backend->watches[0]->promise.set_value(1);
    backend->next_lookup_error = StoreError::NotRunning;
    ASSERT_TRUE(mailbox->poll(now));
    EXPECT_EQ(mailbox->state(), RecoveryState::Reconnecting);
    EXPECT_EQ(statuses, (std::vector<RecoveryState>{RecoveryState::Reconnecting}));
    now += 10ms;
    ASSERT_TRUE(mailbox->poll(now));
    ASSERT_EQ(backend->watches.size(), 2U);
    backend->watches[1]->promise.set_value(1);
    ASSERT_TRUE(mailbox->poll(now));
    ASSERT_EQ(backend->lookups.size(), 1U);
    backend->lookups[0]({StoreBatch{{Bytes{std::byte{7}}}, {}, false}, true});
    drain();
    ASSERT_EQ(mailbox->state(), RecoveryState::Ready);
    EXPECT_EQ(records, (std::vector<Bytes>{Bytes{std::byte{7}}}));
    EXPECT_TRUE(errors.empty());
}

TEST_F(RecoveringMailboxTest, RegistrationFailurePreservesQueuedBackendError) {
    create();
    ASSERT_TRUE(mailbox->poll(now));
    backend->watches[0]->callback(StoreBatch{{}, StoreError::PermanentFailure, false});
    backend->watches[0]->promise.set_value(0);
    EXPECT_FALSE(mailbox->poll(now));
    EXPECT_EQ(mailbox->state(), RecoveryState::Failed);
    EXPECT_EQ(errors, (std::vector<RecoveryError>{RecoveryError::Backend}));
    drain();
    EXPECT_TRUE(statuses.empty());
}

TEST_F(RecoveringMailboxTest, RegistrationFailureRetriesQueuedTransientError) {
    create({1, 10ms, 100ms});
    ASSERT_TRUE(mailbox->poll(now));
    backend->watches[0]->callback(StoreBatch{{}, StoreError::NotRunning, false});
    backend->watches[0]->promise.set_value(0);
    EXPECT_TRUE(mailbox->poll(now));
    EXPECT_EQ(mailbox->state(), RecoveryState::Reconnecting);
    EXPECT_EQ(statuses, (std::vector<RecoveryState>{RecoveryState::Reconnecting}));
    drain();
    EXPECT_TRUE(errors.empty());
}

TEST(NativeMailboxInlineExecutor, InlineExecutorIsDetectedInsteadOfDeadlocking) {
    auto backend = std::make_shared<Store>();
    std::vector<Bytes> records;
    Executor inline_executor = [](std::function<void()> task) { task(); };
    auto created = NativeMailbox::create(
        RecordStore(backend), {std::byte{1}}, {4096, 16, 1024}, MemoryBudget(4096),
        inline_executor,
        {[&](Bytes value) { records.push_back(std::move(value)); },
         [](MailboxError, std::optional<StoreError>) {}, [](bool) {}});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeMailbox>>(created));
    auto mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(created));
    ASSERT_EQ(backend->watches.size(), 1U);
    auto finished = std::async(std::launch::async, [&] {
        backend->watches[0]->callback(StoreBatch{{Bytes{std::byte{7}}}, {}, false});
        backend->watches[0]->callback(StoreBatch{{}, StoreError::PermanentFailure, false});
    });
    ASSERT_EQ(finished.wait_for(10s), std::future_status::ready) << "mailbox deadlocked";
    EXPECT_TRUE(records.empty());
    EXPECT_FALSE(mailbox->is_open());
}
