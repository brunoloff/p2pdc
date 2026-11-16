#include "../src/core/wall_clock.hpp"

#include <gtest/gtest.h>

using namespace p2pdc::detail;

TEST(WallClock, AdvancesAndAbsorbsSmallBackwardSteps) {
    std::atomic<std::uint64_t> last{0};
    EXPECT_EQ(forward_seconds(last, 1000, 30), 1000U);
    EXPECT_EQ(forward_seconds(last, 1005, 30), 1005U);
    EXPECT_EQ(forward_seconds(last, 990, 30), 1005U);
    EXPECT_EQ(forward_seconds(last, 975, 30), 1005U);
    EXPECT_EQ(last.load(), 1005U);
    EXPECT_EQ(forward_seconds(last, 1006, 30), 1006U);
}

TEST(WallClock, AdoptsALargeBackwardStepAsTheNewTime) {
    std::atomic<std::uint64_t> last{0};
    EXPECT_EQ(forward_seconds(last, 1000, 30), 1000U);
    EXPECT_EQ(forward_seconds(last, 969, 30), 969U);
    EXPECT_EQ(last.load(), 969U);
    EXPECT_EQ(forward_seconds(last, 960, 30), 969U);
}

TEST(WallClock, ProcessClockIsNondecreasingAndNearSystemTime) {
    const auto first = forward_utc_seconds();
    const auto second = forward_utc_seconds();
    EXPECT_LE(first, second);
    EXPECT_LE(second, utc_seconds() + clock_regression_tolerance);
    EXPECT_GT(first, 1700000000U);
}
