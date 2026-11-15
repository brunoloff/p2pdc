#include "../src/core/backend_drain.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <stdexcept>
#include <vector>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(BackendDrain, WaitsForNestedTasksBeforeDraining) {
    std::deque<std::function<void()>> queue;
    BackendDrain drain([&](auto task) { queue.push_back(std::move(task)); });
    const auto executor = drain.executor();
    std::vector<int> events;
    executor([&] {
        events.push_back(1);
        executor([&] { events.push_back(2); });
    });
    EXPECT_TRUE(drain.begin([&] { events.push_back(3); }));
    EXPECT_FALSE(drain.begin([] {}));
    while (!queue.empty()) {
        auto task = std::move(queue.front());
        queue.pop_front();
        task();
    }
    EXPECT_EQ(events, (std::vector{1, 2, 3}));
    EXPECT_THROW(executor([] {}), std::logic_error);
}

TEST(BackendDrain, ValidatesInputsAndDrainsAfterException) {
    EXPECT_THROW(BackendDrain({}), std::invalid_argument);
    std::deque<std::function<void()>> queue;
    BackendDrain drain([&](auto task) { queue.push_back(std::move(task)); });
    auto executor = drain.executor();
    EXPECT_THROW(executor({}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(drain.begin({})), std::invalid_argument);
    bool finished = false;
    executor([] { throw std::runtime_error("test failure"); });
    EXPECT_TRUE(drain.begin([&] { finished = true; }));
    auto task = std::move(queue.front());
    queue.pop_front();
    EXPECT_THROW(task(), std::runtime_error);
    EXPECT_TRUE(finished);
}

TEST(BackendDrain, EmptyDrainIsImmediateAndRejectsLateWork) {
    BackendDrain drain([](auto) {});
    bool finished = false;
    EXPECT_TRUE(drain.begin([&] { finished = true; }));
    EXPECT_TRUE(finished);
    EXPECT_THROW(drain.executor()([] {}), std::logic_error);
}

TEST(BackendDrain, RejectedSubmissionDoesNotBlockDrain) {
    BackendDrain drain([](auto) { throw std::runtime_error("executor rejected task"); });
    EXPECT_THROW(drain.executor()([] {}), std::runtime_error);
    bool finished = false;
    EXPECT_TRUE(drain.begin([&] { finished = true; }));
    EXPECT_TRUE(finished);
}
