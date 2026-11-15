#include "p2pdc/memory_budget.h"

#include <gtest/gtest.h>

#include <limits>
#include <thread>
#include <vector>

using namespace p2pdc;

TEST(MemoryBudget, ExhaustionReleaseAndSharedCopies) {
    MemoryBudget budget(10);
    auto copy = budget;
    auto first = budget.try_reserve(7);
    ASSERT_TRUE(first);
    EXPECT_EQ(copy.used(), 7U);
    EXPECT_EQ(copy.limit(), 10U);
    EXPECT_FALSE(copy.try_reserve(4));
    EXPECT_FALSE(copy.try_reserve(std::numeric_limits<std::size_t>::max()));
    auto second = copy.try_reserve(3);
    ASSERT_TRUE(second);
    first->release();
    first->release();
    EXPECT_EQ(budget.used(), 3U);
    second.reset();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(MemoryBudget, ReservationsMoveAndOutliveBudget) {
    std::optional<MemoryReservation> retained;
    {
        MemoryBudget budget(4);
        auto source = budget.try_reserve(4);
        ASSERT_TRUE(source);
        retained = std::move(source);
        EXPECT_EQ(source->bytes(), 0U);
        EXPECT_EQ(retained->bytes(), 4U);
        EXPECT_EQ(budget.used(), 4U);
        auto replacement = budget.try_reserve(0);
        ASSERT_TRUE(replacement);
        *replacement = std::move(*retained);
        retained = std::move(replacement);
        EXPECT_EQ(budget.used(), 4U);
    }
    retained.reset();
}

TEST(MemoryBudget, CompetingReservationsNeverExceedLimit) {
    MemoryBudget budget(10);
    std::vector<std::thread> threads;
    std::atomic<int> successes{0};
    std::vector<std::optional<MemoryReservation>> holds(20);
    for (std::size_t i = 0; i < holds.size(); ++i) {
        threads.emplace_back([&, i] {
            holds[i] = budget.try_reserve(1);
            if (holds[i]) {
                ++successes;
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(successes, 10);
    EXPECT_EQ(budget.used(), 10U);
    holds.clear();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_TRUE(budget.try_reserve(0));
}
