#include "../src/core/submission_gate.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

TEST(SubmissionGate, AcceptsBeforeCloseAndRejectsAfterClose) {
    std::vector<std::function<void()>> tasks;
    SubmissionGate gate([&](auto task) { tasks.push_back(std::move(task)); });
    int count = 0;
    EXPECT_TRUE(gate.post([&] { ++count; }));
    gate.close();
    gate.close();
    EXPECT_FALSE(gate.post([&] { ++count; }));
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_EQ(count, 1);
    EXPECT_THROW(static_cast<void>(gate.post({})), std::invalid_argument);
    EXPECT_THROW((SubmissionGate{{}}), std::invalid_argument);
}

TEST(SubmissionGate, CloseWaitsForSubmissionAlreadyInProgress) {
    std::promise<void> entered, resume;
    auto released = resume.get_future().share();
    SubmissionGate gate([&](auto) {
        entered.set_value();
        released.wait();
    });
    std::thread submitter([&] { EXPECT_TRUE(gate.post([] {})); });
    entered.get_future().wait();
    auto closer = std::async(std::launch::async, [&] { gate.close(); });
    EXPECT_EQ(closer.wait_for(50ms), std::future_status::timeout);
    resume.set_value();
    submitter.join();
    closer.get();
    EXPECT_FALSE(gate.post([] {}));
}

TEST(SubmissionGate, InlineExecutorIsRejectedWithoutRunningTheTask) {
    SubmissionGate gate([](auto task) { task(); });
    int count = 0;
    EXPECT_THROW(static_cast<void>(gate.post([&] { ++count; })), std::logic_error);
    EXPECT_EQ(count, 0);
    gate.close();
    EXPECT_FALSE(gate.post([&] { ++count; }));
}

TEST(SubmissionGate, TaskRunOnAnotherThreadDuringSubmissionIsAllowed) {
    std::vector<std::thread> threads;
    std::promise<void> ran;
    SubmissionGate gate([&](auto task) { threads.emplace_back(std::move(task)); });
    EXPECT_TRUE(gate.post([&] { ran.set_value(); }));
    EXPECT_EQ(ran.get_future().wait_for(10s), std::future_status::ready);
    for (auto &thread : threads)
        thread.join();
}
