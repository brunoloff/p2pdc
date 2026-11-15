#include "../src/core/shutdown_coordinator.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <stdexcept>
#include <vector>

using namespace p2pdc;
using namespace p2pdc::detail;

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
} // namespace

TEST(ShutdownCoordinator, CancelsOperationsAndDrainsBeforeFinalCallback) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    auto gate = coordinator.gate();
    std::vector<int> calls;
    CompletionSource<int> source([gate](auto task) { ASSERT_TRUE(gate.post(std::move(task))); },
                                 [&](auto result) {
                                     EXPECT_EQ(std::get<OperationError>(result),
                                               OperationError::Canceled);
                                     calls.push_back(2);
                                 });
    const auto tracked =
        coordinator.track([operation = source.operation()] { (void)operation.cancel(); });
    ASSERT_TRUE(tracked);
    loop.executor()([&] { calls.push_back(1); });
    EXPECT_TRUE(coordinator.begin([&] { calls.push_back(3); }));
    EXPECT_FALSE(coordinator.track([] {}));
    EXPECT_FALSE(coordinator.retry());
    loop.drain();
    EXPECT_FALSE(gate.post([] {}));
    EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
}

TEST(ShutdownCoordinator, FenceAllowsPreviouslyQueuedCompletionToEnterGate) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    auto gate = coordinator.gate();
    std::vector<int> calls;
    loop.executor()([&] { EXPECT_TRUE(gate.post([&] { calls.push_back(1); })); });
    EXPECT_TRUE(coordinator.begin([&] { calls.push_back(2); }));
    loop.drain();
    EXPECT_EQ(calls, (std::vector<int>{1, 2}));
}

TEST(ShutdownCoordinator, StopCanRetireItsOwnRegistration) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    std::optional<std::uint64_t> id;
    id = coordinator.track([&] {
        EXPECT_FALSE(coordinator.retry());
        coordinator.retire(*id);
    });
    ASSERT_TRUE(id);
    EXPECT_TRUE(coordinator.begin([] {}));
    loop.drain();
}

TEST(ShutdownCoordinator, StopCanRetireAnotherRegistrationBeforeItRuns) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    std::optional<std::uint64_t> second;
    ASSERT_TRUE(coordinator.track([&] { coordinator.retire(*second); }));
    int stopped = 0;
    second = coordinator.track([&] { ++stopped; });
    ASSERT_TRUE(second);
    EXPECT_TRUE(coordinator.begin([] {}));
    loop.drain();
    EXPECT_EQ(stopped, 0);
}

TEST(ShutdownCoordinator, FailedStopKeepsShutdownRetryable) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    int stops = 0;
    bool final = false;
    ASSERT_TRUE(coordinator.track([&] {
        if (++stops == 1) {
            throw std::bad_alloc();
        }
    }));
    EXPECT_THROW(static_cast<void>(coordinator.begin([&] { final = true; })), std::bad_alloc);
    EXPECT_FALSE(coordinator.track([] {}));
    EXPECT_FALSE(final);
    EXPECT_TRUE(coordinator.retry());
    loop.drain();
    EXPECT_TRUE(final);
    EXPECT_EQ(stops, 2);
}

TEST(ShutdownCoordinator, RetriedStopPreservesCallableState) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    bool final = false;
    ASSERT_TRUE(coordinator.track([attempt = 0]() mutable {
        if (++attempt == 1) {
            throw std::bad_alloc();
        }
    }));
    EXPECT_THROW(static_cast<void>(coordinator.begin([&] { final = true; })), std::bad_alloc);
    EXPECT_TRUE(coordinator.retry());
    loop.drain();
    EXPECT_TRUE(final);
}

TEST(ShutdownCoordinator, RetiredActionsAreNotStoppedAndInputsAreChecked) {
    Loop loop;
    ShutdownCoordinator coordinator(loop.executor());
    EXPECT_THROW(static_cast<void>(coordinator.track({})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.begin({})), std::invalid_argument);
    EXPECT_FALSE(coordinator.retry());
    int stops = 0;
    auto id = coordinator.track([&] { ++stops; });
    ASSERT_TRUE(id);
    coordinator.retire(*id);
    EXPECT_TRUE(coordinator.begin([] {}));
    EXPECT_FALSE(coordinator.begin([] {}));
    loop.drain();
    EXPECT_EQ(stops, 0);
}
