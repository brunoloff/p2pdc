#include "p2pdc/runtime.h"
#include "runtime_consumer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace p2pdc;

TEST(RuntimeTest, PortableConsumerHasSameBehavior) {
    auto done = std::make_shared<std::promise<bool>>();
    auto result = done->get_future();
    p2pdc_test::run_runtime_consumer([done](bool value) { done->set_value(value); });
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(result.get());
}

TEST(RuntimeTest, RunsPostedWorkInOrderOnOneWorker) {
    Runtime runtime;
    auto post = runtime.executor();
    std::promise<std::vector<int>> done;
    std::vector<int> order;
    const auto caller = std::this_thread::get_id();
    post([&] { order.push_back(1); });
    post([&] {
        EXPECT_NE(caller, std::this_thread::get_id());
        order.push_back(2);
        done.set_value(order);
    });
    auto result = done.get_future();
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(result.get(), (std::vector<int>{1, 2}));
}

TEST(RuntimeTest, RunsTimerAndRespectsCancellation) {
    Runtime runtime;
    auto schedule = runtime.scheduler();
    std::promise<bool> done;
    bool canceled_ran = false;
    auto cancel = schedule(30ms, [&] { canceled_ran = true; });
    cancel();
    cancel();
    schedule(60ms, [&] { done.set_value(canceled_ran); });
    auto result = done.get_future();
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(result.get());
}

TEST(RuntimeTest, CanceledTimerReleasesCapturesBeforeDeadline) {
    Runtime runtime;
    auto owned = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owned;
    auto cancel = runtime.scheduler()(30s, [owned] {});
    owned.reset();
    ASSERT_FALSE(weak.expired());
    cancel();
    cancel();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!weak.expired() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(weak.expired());
}

TEST(RuntimeTest, CanceledCaptureCanPostDuringDestruction) {
    struct Reentrant {
        Executor post;
        std::shared_ptr<std::promise<void>> done;
        ~Reentrant() {
            post([done = done] { done->set_value(); });
        }
    };
    Runtime runtime;
    auto done = std::make_shared<std::promise<void>>();
    auto result = done->get_future();
    auto captured = std::make_shared<Reentrant>(runtime.executor(), done);
    auto cancel = runtime.scheduler()(30s, [captured] {});
    captured.reset();
    cancel();
    EXPECT_EQ(result.wait_for(5s), std::future_status::ready);
}

TEST(RuntimeTest, CancelAfterRuntimeDestructionIsSafe) {
    CancelTimer cancel;
    {
        Runtime runtime;
        cancel = runtime.scheduler()(30s, [] {});
    }
    EXPECT_NO_THROW(cancel());
    EXPECT_NO_THROW(cancel());
}

TEST(RuntimeTest, RejectsInvalidWorkAndExpiredHandles) {
    Executor post;
    Scheduler schedule;
    {
        Runtime runtime;
        post = runtime.executor();
        schedule = runtime.scheduler();
        EXPECT_THROW(post({}), std::invalid_argument);
        EXPECT_THROW(schedule(-1ms, [] {}), std::invalid_argument);
        EXPECT_THROW(schedule(0ms, {}), std::invalid_argument);
    }
    EXPECT_THROW(post([] {}), std::runtime_error);
    EXPECT_THROW(schedule(0ms, [] {}), std::runtime_error);
}

TEST(RuntimeTest, DueTimersAndQueuedWorkBothMakeProgress) {
    Runtime runtime;
    auto post = runtime.executor();
    auto schedule = runtime.scheduler();
    std::promise<void> entered, release, complete;
    auto gate = release.get_future().share();
    int timers_run = 0;
    int observed = -1;
    post([&] {
        entered.set_value();
        gate.wait();
    });
    auto entered_future = entered.get_future();
    if (entered_future.wait_for(5s) != std::future_status::ready) {
        release.set_value();
        FAIL() << "worker did not start";
    }
    for (int i = 0; i < 4; ++i) {
        schedule(0ms, [&] {
            if (++timers_run == 4) {
                complete.set_value();
            }
        });
    }
    post([&] { observed = timers_run; });
    auto result = complete.get_future();
    release.set_value();
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(observed, 1);
}

TEST(RuntimeTest, CanBeDestroyedFromItsWorker) {
    auto runtime = std::make_shared<std::unique_ptr<Runtime>>(std::make_unique<Runtime>());
    auto post = (*runtime)->executor();
    auto finished = std::make_shared<std::promise<void>>();
    auto result = finished->get_future();
    post([runtime, finished] {
        runtime->reset();
        finished->set_value();
    });
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_THROW(post([] {}), std::runtime_error);
}

TEST(RuntimeTest, ShutdownWaitsForRunningWorkAndDropsPendingWork) {
    auto runtime = std::make_unique<Runtime>();
    auto post = runtime->executor();
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic_bool pending_ran = false;
    post([&] {
        entered.set_value();
        gate.wait();
    });
    auto entered_future = entered.get_future();
    if (entered_future.wait_for(5s) != std::future_status::ready) {
        release.set_value();
        FAIL() << "worker did not start";
    }
    post([&] { pending_ran = true; });
    std::thread stop([&] { runtime.reset(); });
    // Wait for shutdown to reject new submissions before releasing the worker.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool stopped = false;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            post([] {});
        } catch (const std::runtime_error &) {
            stopped = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    release.set_value();
    stop.join();
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(pending_ran.load());
}
