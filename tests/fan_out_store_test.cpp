#include "../src/backends/fan_out_store.hpp"
#include "store_sync.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace p2pdc::test;

namespace {

struct FakeWatch final : RecordWatchBackend {
    explicit FakeWatch(std::shared_future<std::size_t> token) : token_(std::move(token)) {}
    void cancel() override { canceled = true; }
    std::shared_future<std::size_t> registration() const override { return token_; }
    std::shared_future<std::size_t> token_;
    std::atomic<bool> canceled{false};
};

// A store that answers as told, on a thread of its own when delay is set.
struct FakeStore final : RecordStoreBackend {
    bool put_ok = true;
    std::optional<StoreError> refuse;
    LookupResult answer{{}, true};
    std::chrono::milliseconds delay{0};
    std::size_t token = 1; // what watches register with
    mutable std::mutex mutex;
    mutable std::vector<std::function<void(StoreBatch)>> watchers;
    mutable std::atomic<int> watches{0};

    template <class Task> void later(Task task) const {
        if (delay.count() == 0)
            task();
        else
            std::thread([task, wait = delay] {
                std::this_thread::sleep_for(wait);
                task();
            }).detach();
    }
    std::optional<StoreError> put(ByteView, ByteView,
                                  std::function<void(bool)> done) const override {
        if (refuse)
            return refuse;
        later([done, ok = put_ok] { done(ok); });
        return std::nullopt;
    }
    std::optional<StoreError> lookup(ByteView, std::function<void(LookupResult)> done,
                                     LookupDeduplication) const override {
        if (refuse)
            return refuse;
        later([done, result = answer] { done(result); });
        return std::nullopt;
    }
    std::variant<RecordWatch, StoreError> watch(ByteView, std::function<void(StoreBatch)> callback,
                                                bool) const override {
        if (refuse)
            return *refuse;
        ++watches;
        {
            std::lock_guard lock(mutex);
            watchers.push_back(callback);
        }
        std::promise<std::size_t> ready;
        ready.set_value(token);
        return RecordWatch(std::make_unique<FakeWatch>(ready.get_future().share()));
    }
    void send(StoreBatch batch) const {
        std::lock_guard lock(mutex);
        watchers.back()(std::move(batch));
    }
};

class FanOutStoreTest : public ::testing::Test {
  protected:
    RecordStore fan_out(std::initializer_list<std::shared_ptr<FakeStore>> fakes) {
        std::vector<RecordStore> stores;
        for (const auto &fake : fakes)
            stores.emplace_back(fake);
        return make_fan_out_store(loop, std::move(stores), {64, 8, 1024}, timing);
    }
    void TearDown() override { loop->stop(); }
    std::shared_ptr<BackendLoop> loop = std::make_shared<BackendLoop>();
    FanOutTiming timing{100ms, 50ms, 200ms, 10ms};
    std::shared_ptr<FakeStore> a = std::make_shared<FakeStore>(), b = std::make_shared<FakeStore>(),
                               c = std::make_shared<FakeStore>();
};

} // namespace

TEST_F(FanOutStoreTest, APutSucceedsWhenAnyStoreAcceptsIt) {
    a->put_ok = false;
    b->refuse = StoreError::NotRunning;
    EXPECT_EQ(put(fan_out({a, b, c}), test_key(1), bytes({1})), true);
    c->put_ok = false;
    EXPECT_EQ(put(fan_out({a, b, c}), test_key(1), bytes({1})), false);
    a->refuse = c->refuse = StoreError::ResourceLimit;
    b->refuse = StoreError::ResourceLimit;
    EXPECT_EQ(fan_out({a, b, c}).put(test_key(1), bytes({1}), [](bool) { FAIL(); }),
              StoreError::ResourceLimit);
    EXPECT_THROW(static_cast<void>(make_fan_out_store(loop, {RecordStore(a)}, {64, 8, 1024})),
                 std::invalid_argument);
}

TEST_F(FanOutStoreTest, ALookupMergesAnswersWithoutWaitingLongForSlowStores) {
    a->answer.batch.values = {bytes({1}), bytes({2})};
    b->answer.batch.values = {bytes({2}), bytes({3})};
    b->answer.batch.dropped = 1;
    c->delay = 2s;
    const auto started = std::chrono::steady_clock::now();
    const auto found = lookup(fan_out({a, b, c}), test_key(2));
    ASSERT_TRUE(found);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_TRUE(found->backend_success);
    EXPECT_EQ(sorted(found->batch.values),
              sorted({bytes({1}), bytes({2}), bytes({2}), bytes({3})}));
    EXPECT_EQ(found->batch.dropped, 1U);
    const auto merged = lookup(fan_out({a, b}), test_key(2), LookupDeduplication::Content);
    ASSERT_TRUE(merged);
    EXPECT_EQ(sorted(merged->batch.values), sorted({bytes({1}), bytes({2}), bytes({3})}));
}

TEST_F(FanOutStoreTest, ALookupFailsOnlyWhenNoStoreAnswers) {
    a->answer = {StoreBatch{{}, StoreError::PermanentFailure}, false};
    b->refuse = StoreError::NotRunning;
    const auto found = lookup(fan_out({a, b}), test_key(3));
    ASSERT_TRUE(found);
    EXPECT_FALSE(found->backend_success);
    EXPECT_EQ(found->batch.error, StoreError::PermanentFailure);
}

TEST_F(FanOutStoreTest, AWatchIsReadyOnceOneStoreIsAndWatchesAFailedOneAgain) {
    b->token = 0; // registration fails
    Heard heard;
    auto result = fan_out({a, b}).watch(test_key(4), heard.callback(), false);
    auto &watch = std::get<RecordWatch>(result);
    EXPECT_EQ(registered(watch), 1U);
    a->send(StoreBatch{{bytes({7})}, {}, false, 0});
    ASSERT_TRUE(heard.wait_values(1));
    for (int wait = 0; wait < 100 && b->watches < 3; ++wait)
        std::this_thread::sleep_for(10ms);
    EXPECT_GE(b->watches, 3) << "watched again after each failure";
    b->token = 1;
    a->send(StoreBatch{{}, StoreError::AllocationFailed, false, 0});
    for (int wait = 0; wait < 100 && a->watches < 2; ++wait)
        std::this_thread::sleep_for(10ms);
    EXPECT_GE(a->watches, 2);
    EXPECT_TRUE(heard.errors().empty()) << "a failure while others carry on stays inside";
    watch.cancel();
    a->send(StoreBatch{{bytes({8})}, {}, false, 0});
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(heard.values(), std::vector<Bytes>{bytes({7})});
}

TEST_F(FanOutStoreTest, AWatchFailsWhenEveryStoreFailsBeforeAnyIsReady) {
    a->token = 0;
    b->refuse = StoreError::ResourceLimit;
    Heard heard;
    auto result = fan_out({a, b}).watch(test_key(5), heard.callback(), false);
    EXPECT_EQ(registered(std::get<RecordWatch>(result)), 0U);
    ASSERT_TRUE(heard.wait_error());
    EXPECT_EQ(heard.errors().size(), 1U);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(a->watches, 1) << "not watched again";
}
