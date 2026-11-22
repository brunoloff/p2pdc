#include "../src/core/deferred_events.hpp"
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
    void step() {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
    void drain() {
        while (!tasks.empty())
            step();
    }
};
} // namespace

TEST(DeferredEvents, DeliversInOrderWithinCountAndByteBudgets) {
    Loop loop;
    MemoryBudget budget(5);
    DeferredEvents events(loop.executor(), budget, 2, 5);
    std::vector<int> calls;
    EXPECT_EQ(events.post(3, [&] { calls.push_back(1); }), DeferredEventResult::Accepted);
    EXPECT_EQ(events.post(2, [&] { calls.push_back(2); }), DeferredEventResult::Accepted);
    EXPECT_EQ(events.post(0, [] {}), DeferredEventResult::Full);
    EXPECT_EQ(budget.used(), 5U);
    EXPECT_TRUE(calls.empty());
    ASSERT_EQ(loop.tasks.size(), 1U);
    loop.step();
    EXPECT_EQ(calls, (std::vector<int>{1}));
    EXPECT_EQ(events.pending(), 1U);
    EXPECT_EQ(budget.used(), 2U);
    loop.drain();
    EXPECT_EQ(calls, (std::vector<int>{1, 2}));
    EXPECT_EQ(events.pending(), 0U);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DeferredEvents, CloseSuppressesQueuedCallbacksAndReleasesReservations) {
    Loop loop;
    MemoryBudget budget(8);
    DeferredEvents events(loop.executor(), budget, 4, 8);
    int calls = 0;
    ASSERT_EQ(events.post(4, [&] { ++calls; }), DeferredEventResult::Accepted);
    events.close();
    events.close();
    EXPECT_EQ(events.post(1, [&] { ++calls; }), DeferredEventResult::Closed);
    EXPECT_EQ(events.pending(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    loop.drain();
    EXPECT_EQ(calls, 0);
}

TEST(DeferredEvents, CallbackMayDestroyQueueAndSuppressLaterEvents) {
    Loop loop;
    MemoryBudget budget(8);
    auto events = std::make_unique<DeferredEvents>(loop.executor(), budget, 4, 8);
    int calls = 0;
    ASSERT_EQ(events->post(2,
                           [&] {
                               ++calls;
                               events.reset();
                           }),
              DeferredEventResult::Accepted);
    ASSERT_EQ(events->post(2, [&] { ++calls; }), DeferredEventResult::Accepted);
    loop.drain();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(events);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DeferredEvents, CallbackMayCloseQueueAndSuppressAlreadyQueuedEvents) {
    Loop loop;
    MemoryBudget budget(8);
    DeferredEvents events(loop.executor(), budget, 4, 8);
    int calls = 0;
    ASSERT_EQ(events.post(2,
                          [&] {
                              ++calls;
                              events.close();
                          }),
              DeferredEventResult::Accepted);
    ASSERT_EQ(events.post(2, [&] { ++calls; }), DeferredEventResult::Accepted);
    loop.drain();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(events.pending(), 0U);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DeferredEvents, DiscardedCaptureMayReenterClose) {
    struct CloseOnDrop {
        DeferredEvents *events;
        ~CloseOnDrop() { events->close(); }
    };
    Loop loop;
    MemoryBudget budget(8);
    DeferredEvents events(loop.executor(), budget, 4, 8);
    auto capture = std::make_shared<CloseOnDrop>(&events);
    ASSERT_EQ(events.post(2, [capture] {}), DeferredEventResult::Accepted);
    capture.reset();
    ASSERT_EQ(events.post(2, [] {}), DeferredEventResult::Accepted);
    events.close();
    EXPECT_EQ(events.pending(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    loop.drain();
}

TEST(DeferredEvents, DiscardedCaptureMayDestroyQueueDuringClose) {
    struct DestroyOnDrop {
        std::unique_ptr<DeferredEvents> *owner;
        ~DestroyOnDrop() { owner->reset(); }
    };
    Loop loop;
    MemoryBudget budget(8);
    auto events = std::make_unique<DeferredEvents>(loop.executor(), budget, 4, 8);
    auto capture = std::make_shared<DestroyOnDrop>(&events);
    ASSERT_EQ(events->post(2, [capture] {}), DeferredEventResult::Accepted);
    capture.reset();
    ASSERT_EQ(events->post(2, [] {}), DeferredEventResult::Accepted);
    auto *raw = events.get();
    raw->close();
    EXPECT_FALSE(events);
    EXPECT_EQ(budget.used(), 0U);
    loop.drain();
}

TEST(DeferredEvents, CallbackMayEnqueueAndRunningEventCountsAgainstLimit) {
    Loop loop;
    MemoryBudget budget(8);
    DeferredEvents events(loop.executor(), budget, 1, 8);
    DeferredEventResult reentrant = DeferredEventResult::Accepted;
    ASSERT_EQ(events.post(1, [&] { reentrant = events.post(1, [] {}); }),
              DeferredEventResult::Accepted);
    loop.drain();
    EXPECT_EQ(reentrant, DeferredEventResult::Full);
    EXPECT_EQ(events.post(1, [] {}), DeferredEventResult::Accepted);
    loop.drain();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DeferredEvents, CallbackExceptionClosesQueueAndReleasesReservations) {
    Loop loop;
    MemoryBudget budget(8);
    DeferredEvents events(loop.executor(), budget, 4, 8);
    ASSERT_EQ(events.post(2, [] { throw std::runtime_error("test"); }),
              DeferredEventResult::Accepted);
    ASSERT_EQ(events.post(2, [] {}), DeferredEventResult::Accepted);
    EXPECT_THROW(loop.step(), std::runtime_error);
    EXPECT_EQ(events.pending(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(events.post(0, [] {}), DeferredEventResult::Closed);
}

TEST(DeferredEvents, RejectsInvalidInputsAndSharedBudgetExhaustion) {
    Loop loop;
    EXPECT_THROW((DeferredEvents({}, MemoryBudget(8), 1, 8)), std::invalid_argument);
    EXPECT_THROW((DeferredEvents(loop.executor(), MemoryBudget(8), 0, 8)), std::invalid_argument);
    MemoryBudget budget(2);
    DeferredEvents first(loop.executor(), budget, 2, 8), second(loop.executor(), budget, 2, 8);
    EXPECT_THROW((void)first.post(0, {}), std::invalid_argument);
    ASSERT_EQ(first.post(2, [] {}), DeferredEventResult::Accepted);
    EXPECT_EQ(second.post(1, [] {}), DeferredEventResult::Full);
    loop.drain();
    EXPECT_EQ(second.post(1, [] {}), DeferredEventResult::Accepted);
    loop.drain();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DeferredEvents, SubmissionFailureClosesQueueAndReleasesBudget) {
    MemoryBudget budget(8);
    DeferredEvents events([](auto) { throw std::bad_alloc(); }, budget, 2, 8);
    EXPECT_THROW((void)events.post(4, [] {}), std::bad_alloc);
    EXPECT_EQ(events.pending(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(events.post(1, [] {}), DeferredEventResult::Closed);
}

TEST(DeferredEvents, ShutdownDrainsSuppressedWorkBeforeFinalCallback) {
    Loop loop;
    auto events = std::make_shared<DeferredEvents>(loop.executor(), MemoryBudget(8), 2, 8);
    ShutdownCoordinator shutdown(loop.executor());
    ASSERT_TRUE(shutdown.track([events] { events->close(); }));
    int calls = 0;
    ASSERT_EQ(events->post(1, [&] { ++calls; }), DeferredEventResult::Accepted);
    bool finished = false;
    ASSERT_TRUE(shutdown.begin([&] { finished = true; }));
    loop.drain();
    EXPECT_TRUE(finished);
    EXPECT_EQ(calls, 0);
}

TEST(DeferredEvents, HasRoomKeepsASlotSpareAndRespectsBytes) {
    Loop loop;
    MemoryBudget budget(8);
    DeferredEvents events(loop.executor(), budget, 3, 6);
    EXPECT_TRUE(events.has_room(6));
    EXPECT_FALSE(events.has_room(7));
    ASSERT_EQ(events.post(2, [] {}), DeferredEventResult::Accepted);
    EXPECT_TRUE(events.has_room(4));
    EXPECT_FALSE(events.has_room(5)); // only 4 bytes left
    ASSERT_EQ(events.post(0, [] {}), DeferredEventResult::Accepted);
    EXPECT_FALSE(events.has_room(0)); // the last slot stays spare
    loop.drain();
    EXPECT_TRUE(events.has_room(6));
    events.close();
    EXPECT_FALSE(events.has_room(0));
}

TEST(DeferredEvents, HasRoomTakesOneEventWhenOnlyOneFits) {
    Loop loop;
    DeferredEvents events(loop.executor(), MemoryBudget(4), 1, 4);
    EXPECT_TRUE(events.has_room(4));
    ASSERT_EQ(events.post(4, [] {}), DeferredEventResult::Accepted);
    EXPECT_FALSE(events.has_room(0));
}
