#include "p2pdc/callback_gate.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace p2pdc;

TEST(CallbackGate, ShutdownFollowsAllAdmittedCallbacks) {
    std::vector<std::function<void()>> tasks;
    std::vector<int> calls;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    EXPECT_TRUE(gate.post([&] { calls.push_back(1); }));
    EXPECT_TRUE(gate.post([&] { calls.push_back(2); }));
    EXPECT_TRUE(gate.close([&] { calls.push_back(3); }));
    EXPECT_FALSE(gate.post([] {}));
    EXPECT_FALSE(gate.close([] {}));
    ASSERT_EQ(tasks.size(), 2U);
    auto first = std::move(tasks[0]);
    auto second = std::move(tasks[1]);
    first();
    second();
    ASSERT_EQ(tasks.size(), 3U);
    tasks.back()();
    EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
}

TEST(CallbackGate, AbandonSuppressesQueuedCallbacksAndRejectsNewWork) {
    std::vector<std::function<void()>> tasks;
    int calls = 0;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    EXPECT_TRUE(gate.post([&] { ++calls; }));
    EXPECT_TRUE(gate.close([&] { ++calls; }));
    gate.abandon();
    gate.abandon();
    EXPECT_FALSE(gate.post([] {}));
    EXPECT_FALSE(gate.close([] {}));
    auto task = std::move(tasks.front());
    task();
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(calls, 0);
}

TEST(CallbackGate, AbandonSuppressesAlreadyScheduledFinalCallback) {
    std::vector<std::function<void()>> tasks;
    bool closed = false;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    EXPECT_TRUE(gate.close([&] { closed = true; }));
    gate.abandon();
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_FALSE(closed);
}

TEST(CallbackGate, AbandonAllowsAlreadyRunningCallbackToFinish) {
    std::vector<std::function<void()>> tasks;
    std::promise<void> entered;
    std::promise<void> resume;
    auto released = resume.get_future().share();
    int calls = 0;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    EXPECT_TRUE(gate.post([&] { entered.set_value(); released.wait(); ++calls; }));
    auto task = std::move(tasks.front());
    std::thread worker(std::move(task));
    entered.get_future().wait();
    gate.abandon();
    resume.set_value();
    worker.join();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(gate.post([] {}));
}

TEST(CallbackGate, CloseDuringCallbackIsDeferredAndArgumentsAreValidated) {
    EXPECT_THROW((CallbackGate{{}}), std::invalid_argument);
    std::vector<std::function<void()>> tasks;
    int calls = 0;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    EXPECT_THROW(static_cast<void>(gate.post({})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(gate.close({})), std::invalid_argument);
    EXPECT_TRUE(gate.post([&] { EXPECT_TRUE(gate.close([&] { ++calls; })); }));
    auto task = std::move(tasks.front());
    task();
    EXPECT_EQ(calls, 0);
    ASSERT_EQ(tasks.size(), 2U);
    tasks.back()();
    EXPECT_EQ(calls, 1);
}

TEST(CallbackGate, ThrowingCallbackStillReleasesPendingDelivery) {
    std::vector<std::function<void()>> tasks;
    bool closed = false;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    EXPECT_TRUE(gate.post([] { throw std::runtime_error("application callback failed"); }));
    EXPECT_TRUE(gate.close([&] { closed = true; }));
    auto task = std::move(tasks.front());
    EXPECT_THROW(task(), std::runtime_error);
    ASSERT_EQ(tasks.size(), 2U);
    tasks.back()();
    EXPECT_TRUE(closed);
}

TEST(CallbackGate, FinalCallbackCanDestroyGateDuringSubmission) {
    std::unique_ptr<CallbackGate> gate;
    gate = std::make_unique<CallbackGate>([](auto task) {
        std::thread worker(std::move(task));
        worker.join();
    });
    const auto accepted = gate->close([&] { gate.reset(); });
    EXPECT_TRUE(accepted);
    EXPECT_FALSE(gate);
}

TEST(CallbackGate, ShutdownWaitsForSubmissionAlreadyInFlight) {
    std::mutex mutex;
    std::condition_variable condition;
    bool entering = false;
    bool resume = false;
    std::vector<std::function<void()>> tasks;
    CallbackGate gate([&](auto task) {
        std::unique_lock lock(mutex);
        if (!entering) {
            entering = true;
            condition.notify_all();
            condition.wait(lock, [&] { return resume; });
        }
        tasks.push_back(std::move(task));
    });
    std::vector<int> calls;
    std::thread backend([&] { EXPECT_TRUE(gate.post([&] { calls.push_back(1); })); });
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entering; });
    }
    EXPECT_TRUE(gate.close([&] { calls.push_back(2); }));
    {
        std::lock_guard lock(mutex);
        EXPECT_TRUE(tasks.empty());
        resume = true;
    }
    condition.notify_all();
    backend.join();
    ASSERT_EQ(tasks.size(), 1U);
    auto task = std::move(tasks.front());
    task();
    ASSERT_EQ(tasks.size(), 2U);
    tasks.back()();
    EXPECT_EQ(calls, (std::vector<int>{1, 2}));
}

TEST(CallbackGate, ShutdownCannotOvertakeOperationSubmission) {
    using namespace std::chrono_literals;
    std::vector<std::function<void()>> tasks;
    std::vector<int> calls;
    CallbackGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    std::promise<void> entered;
    std::promise<void> resume;
    auto released = resume.get_future().share();
    CompletionSource<Unit> source(
        [&](auto task) {
            entered.set_value();
            released.wait();
            EXPECT_TRUE(gate.post(std::move(task)));
        },
        [&](auto) { calls.push_back(1); });
    auto handle = source.operation();
    std::thread backend([&] { EXPECT_TRUE(source.complete(Unit{})); });
    entered.get_future().wait();
    std::promise<void> cancel_started;
    auto shutdown = std::async(std::launch::async, [&] {
        cancel_started.set_value();
        EXPECT_FALSE(handle.cancel());
        return gate.close([&] { calls.push_back(2); });
    });
    cancel_started.get_future().wait();
    EXPECT_EQ(shutdown.wait_for(50ms), std::future_status::timeout);
    resume.set_value();
    backend.join();
    EXPECT_TRUE(shutdown.get());
    ASSERT_EQ(tasks.size(), 1U);
    auto task = std::move(tasks.front());
    task();
    ASSERT_EQ(tasks.size(), 2U);
    tasks.back()();
    EXPECT_EQ(calls, (std::vector<int>{1, 2}));
}
