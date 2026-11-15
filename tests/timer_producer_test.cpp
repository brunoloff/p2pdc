#include "../src/core/shutdown_coordinator.hpp"
#include "../src/core/timer_producer.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
struct Loop {
    std::deque<std::function<void()>> tasks;
    Executor executor() {
        return [this](auto task) { tasks.push_back(std::move(task)); };
    }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
};

struct FakeScheduler {
    std::vector<std::function<void()>> callbacks;
    unsigned cancellations = 0;
    Scheduler scheduler() {
        return [this](auto, auto callback) -> CancelTimer {
            callbacks.push_back(std::move(callback));
            return [this] { ++cancellations; };
        };
    }
};
} // namespace

TEST(TimerProducer, DeliversOnceAndCancelsQueuedDelivery) {
    Loop loop;
    FakeScheduler scheduler;
    TimerProducer timer(loop.executor(), scheduler.scheduler());
    int calls = 0;
    timer.arm(1ms, [&] { ++calls; });
    scheduler.callbacks[0]();
    scheduler.callbacks[0]();
    ASSERT_EQ(loop.tasks.size(), 1U);
    loop.drain();
    EXPECT_EQ(calls, 1);
    timer.arm(1ms, [&] { ++calls; });
    scheduler.callbacks[1]();
    timer.cancel();
    loop.drain();
    EXPECT_EQ(calls, 1);
    scheduler.callbacks[1]();
    EXPECT_TRUE(loop.tasks.empty());
    EXPECT_EQ(scheduler.cancellations, 2U);
}

TEST(TimerProducer, ReplacementAndCloseSuppressLateFirings) {
    Loop loop;
    FakeScheduler scheduler;
    TimerProducer timer(loop.executor(), scheduler.scheduler());
    int first = 0, second = 0;
    timer.arm(1ms, [&] { ++first; });
    timer.arm(2ms, [&] { ++second; });
    scheduler.callbacks[0]();
    scheduler.callbacks[1]();
    ASSERT_EQ(loop.tasks.size(), 1U);
    loop.drain();
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 1);
    timer.close();
    timer.close();
    scheduler.callbacks[1]();
    EXPECT_TRUE(loop.tasks.empty());
    EXPECT_FALSE(timer.healthy());
    EXPECT_THROW(timer.arm(1ms, [] {}), std::logic_error);
}

TEST(TimerProducer, ValidatesSchedulerAndSuppressesFailedRegistration) {
    Loop loop;
    EXPECT_THROW((TimerProducer(loop.executor(), {})), std::invalid_argument);
    Scheduler invalid = [](auto, auto callback) -> CancelTimer {
        callback();
        return {};
    };
    TimerProducer timer(loop.executor(), invalid);
    int calls = 0;
    EXPECT_THROW(timer.arm(-1ms, [] {}), std::invalid_argument);
    EXPECT_THROW(timer.arm(1ms, {}), std::invalid_argument);
    EXPECT_THROW(timer.arm(1ms, [&] { ++calls; }), std::invalid_argument);
    loop.drain();
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(timer.healthy());
}

TEST(TimerProducer, ShutdownFencePrecedesFinalCallbackAndRejectsLateFire) {
    Loop loop;
    FakeScheduler scheduler;
    auto timer = std::make_shared<TimerProducer>(loop.executor(), scheduler.scheduler());
    ShutdownCoordinator shutdown(loop.executor());
    ASSERT_TRUE(shutdown.track([timer] { timer->close(); }));
    int calls = 0;
    timer->arm(1ms, [&] { ++calls; });
    scheduler.callbacks[0]();
    bool finished = false;
    ASSERT_TRUE(shutdown.begin([&] { finished = true; }));
    loop.drain();
    EXPECT_TRUE(finished);
    EXPECT_EQ(calls, 0);
    scheduler.callbacks[0]();
    EXPECT_TRUE(loop.tasks.empty());
}

TEST(TimerProducer, SchedulerFailureCancelsOldTimerAndAllowsRetry) {
    Loop loop;
    FakeScheduler scheduler;
    auto delegate = scheduler.scheduler();
    bool fail = false;
    Scheduler sometimes_fails = [&](auto delay, auto callback) -> CancelTimer {
        if (fail)
            throw std::runtime_error("scheduler unavailable");
        return delegate(delay, std::move(callback));
    };
    TimerProducer timer(loop.executor(), sometimes_fails);
    int calls = 0;
    timer.arm(1ms, [&] { ++calls; });
    fail = true;
    EXPECT_THROW(timer.arm(1ms, [&] { ++calls; }), std::runtime_error);
    scheduler.callbacks[0]();
    loop.drain();
    EXPECT_EQ(calls, 0);
    fail = false;
    timer.arm(1ms, [&] { ++calls; });
    scheduler.callbacks[1]();
    loop.drain();
    EXPECT_EQ(calls, 1);
}

TEST(TimerProducer, ImmediateSchedulerFireStillDeliversAsynchronously) {
    Loop loop;
    Scheduler immediate = [](auto, auto callback) -> CancelTimer {
        callback();
        return [] {};
    };
    TimerProducer timer(loop.executor(), immediate);
    int calls = 0;
    timer.arm(1ms, [&] { ++calls; });
    EXPECT_EQ(calls, 0);
    loop.drain();
    EXPECT_EQ(calls, 1);
}

TEST(TimerProducer, DeliveredCallbackMayRearmTimer) {
    Loop loop;
    FakeScheduler scheduler;
    TimerProducer timer(loop.executor(), scheduler.scheduler());
    int calls = 0;
    timer.arm(1ms, [&] {
        ++calls;
        timer.arm(1ms, [&] { ++calls; });
    });
    scheduler.callbacks[0]();
    loop.drain();
    ASSERT_EQ(scheduler.callbacks.size(), 2U);
    scheduler.callbacks[1]();
    loop.drain();
    EXPECT_EQ(calls, 2);
}

TEST(TimerProducer, CloseWaitsForAnInFlightBackendSubmission) {
    FakeScheduler scheduler;
    std::promise<void> entered, release;
    auto resumed = release.get_future().share();
    std::vector<std::function<void()>> tasks;
    Executor executor = [&](auto task) {
        entered.set_value();
        resumed.wait();
        tasks.push_back(std::move(task));
    };
    TimerProducer timer(executor, scheduler.scheduler());
    int calls = 0;
    timer.arm(1ms, [&] { ++calls; });
    std::thread backend([&] { scheduler.callbacks[0](); });
    entered.get_future().wait();
    auto closing = std::async(std::launch::async, [&] { timer.close(); });
    EXPECT_EQ(closing.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    backend.join();
    closing.get();
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_EQ(calls, 0);
    scheduler.callbacks[0]();
    EXPECT_EQ(tasks.size(), 1U);
}
