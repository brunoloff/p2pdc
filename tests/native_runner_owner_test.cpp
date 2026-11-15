#include "../src/discovery/native_runner_owner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class Queue {
  public:
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
            ready_.notify_one();
        };
    }
    bool run_one() {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, 10s, [&] { return !tasks_.empty(); })) {
            return false;
        }
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
        return true;
    }

  private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
};
} // namespace

TEST(NativeRunnerOwner, StartsRealLocalRunnerAndJoinsBeforeShutdownCallback) {
    Queue queue;
    std::shared_ptr<dht::DhtRunner> runner;
    auto owner = NativeRunnerOwner::create(
        queue.executor(),
        [](dht::DhtRunner &backend) {
            dht::DhtRunner::Config config;
            config.dht_config.node_config.network = 0x12345692;
            config.peer_discovery = false;
            config.peer_publish = false;
            backend.run("127.0.0.1", "::1", "0", config);
        },
        [&](RunnerStartResult result) {
            runner = std::get<std::shared_ptr<dht::DhtRunner>>(result);
        });
    ASSERT_TRUE(queue.run_one());
    ASSERT_TRUE(runner);
    EXPECT_TRUE(runner->isRunning());
    bool stopped = false;
    ASSERT_TRUE(owner->shutdown([&](std::exception_ptr error) {
        EXPECT_FALSE(error);
        EXPECT_FALSE(runner->isRunning());
        stopped = true;
    }));
    EXPECT_FALSE(owner->shutdown([](auto) {}));
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(stopped);
}

TEST(NativeRunnerOwner, ReportsStartupFailureAndCanStillShutdown) {
    Queue queue;
    bool failed = false;
    auto owner = NativeRunnerOwner::create(
        queue.executor(), [](dht::DhtRunner &) { throw std::runtime_error("start failed"); },
        [&](RunnerStartResult result) {
            failed = std::holds_alternative<std::exception_ptr>(result);
        });
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(failed);
    bool stopped = false;
    ASSERT_TRUE(owner->shutdown([&](auto error) { stopped = !error; }));
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(stopped);
}

TEST(NativeRunnerOwner, ShutdownDuringStartupReportsCanceledBeforeStopped) {
    Queue queue;
    std::promise<void> entered;
    std::promise<void> resume;
    auto release = resume.get_future().share();
    std::vector<int> events;
    auto owner = NativeRunnerOwner::create(
        queue.executor(),
        [&](dht::DhtRunner &) {
            entered.set_value();
            release.wait();
        },
        [&](RunnerStartResult result) {
            EXPECT_TRUE(std::holds_alternative<RunnerCanceled>(result));
            events.push_back(1);
        });
    ASSERT_EQ(entered.get_future().wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(owner->shutdown([&](auto error) {
        EXPECT_FALSE(error);
        events.push_back(2);
    }));
    resume.set_value();
    ASSERT_TRUE(queue.run_one());
    ASSERT_TRUE(queue.run_one());
    EXPECT_EQ(events, (std::vector<int>{1, 2}));
}

TEST(NativeRunnerOwner, ValidatesArguments) {
    Queue queue;
    EXPECT_THROW(static_cast<void>(NativeRunnerOwner::create(
                     {}, [](auto &) {}, [](auto) {})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(NativeRunnerOwner::create(queue.executor(), {}, [](auto) {})),
                 std::invalid_argument);
}

TEST(NativeRunnerOwner, SubmissionFailureIsObservableAndDoesNotTerminateWorker) {
    auto owner = NativeRunnerOwner::create([](auto) { throw std::bad_alloc(); },
                                           [](dht::DhtRunner &) {}, [](auto) {});
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!owner->delivery_failed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(owner->delivery_failed());
    EXPECT_TRUE(owner->shutdown([](auto) {}));
}

TEST(NativeRunnerOwner, DestructionDoesNotWaitForBlockedStartup) {
    auto queue = std::make_shared<Queue>();
    auto entered = std::make_shared<std::promise<void>>();
    auto resume = std::make_shared<std::promise<void>>();
    auto exited = std::make_shared<std::promise<void>>();
    auto release = resume->get_future().share();
    auto owner =
        NativeRunnerOwner::create([queue](auto task) { queue->executor()(std::move(task)); },
                                  [entered, exited, release](dht::DhtRunner &) {
                                      entered->set_value();
                                      release.wait();
                                      exited->set_value();
                                  },
                                  [](auto) { ADD_FAILURE() << "abandoned startup callback ran"; });
    ASSERT_EQ(entered->get_future().wait_for(10s), std::future_status::ready);
    owner.reset();
    resume->set_value();
    EXPECT_EQ(exited->get_future().wait_for(10s), std::future_status::ready);
}

TEST(NativeRunnerOwner, QuotaStaysReservedUntilAbandonedWorkerExits) {
    auto queue = std::make_shared<Queue>();
    auto entered = std::make_shared<std::promise<void>>();
    auto resume = std::make_shared<std::promise<void>>();
    auto release = resume->get_future().share();
    RunnerQuota quota(1);
    auto executor = [queue](auto task) { queue->executor()(std::move(task)); };
    auto start = [entered, release](dht::DhtRunner &) {
        entered->set_value();
        release.wait();
    };
    auto owner = NativeRunnerOwner::create(executor, start, [](auto) {}, quota);
    ASSERT_TRUE(owner);
    ASSERT_EQ(entered->get_future().wait_for(10s), std::future_status::ready);
    owner.reset();
    EXPECT_EQ(quota.used(), 1U);
    EXPECT_FALSE(NativeRunnerOwner::create(executor, [](auto &) {}, [](auto) {}, quota));
    resume->set_value();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (quota.used() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(quota.used(), 0U);
    owner = NativeRunnerOwner::create(executor, [](auto &) {}, [](auto) {}, quota);
    ASSERT_TRUE(owner);
    EXPECT_TRUE(owner->shutdown([](auto) {}));
    ASSERT_TRUE(queue->run_one());
    ASSERT_TRUE(queue->run_one());
}

TEST(NativeRunnerOwner, RejectsZeroWorkerQuota) {
    EXPECT_THROW(static_cast<void>(RunnerQuota(0)), std::invalid_argument);
}

TEST(NativeRunnerOwner, FailedStartupKeepsQuotaUntilWorkerStops) {
    Queue queue;
    RunnerQuota quota(1);
    bool failed = false;
    auto owner = NativeRunnerOwner::create(
        queue.executor(), [](auto &) { throw std::runtime_error("failed"); },
        [&](auto result) { failed = std::holds_alternative<std::exception_ptr>(result); }, quota);
    ASSERT_TRUE(owner);
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(failed);
    EXPECT_EQ(quota.used(), 1U);
    EXPECT_FALSE(NativeRunnerOwner::create(queue.executor(), [](auto &) {}, [](auto) {}, quota));
    ASSERT_TRUE(owner->shutdown([](auto error) { EXPECT_FALSE(error); }));
    ASSERT_TRUE(queue.run_one());
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (quota.used() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(quota.used(), 0U);
}
