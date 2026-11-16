#include "../src/discovery/join_retry.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

using namespace p2pdc::detail;
using namespace std::chrono_literals;

TEST(JoinRetry, RetriesWithBackoffAndReturnsTheFirstFailure) {
    int attempts = 0;
    std::vector<std::chrono::milliseconds> delays;
    auto failure = join_with_retry(
        [&] {
            if (++attempts < 9)
                throw std::runtime_error(attempts == 1 ? "first" : "later");
        },
        [&](std::chrono::milliseconds delay) { delays.push_back(delay); });
    EXPECT_EQ(attempts, 9);
    EXPECT_EQ(delays, (std::vector<std::chrono::milliseconds>{100ms, 200ms, 400ms, 800ms, 1600ms,
                                                              3200ms, 5000ms, 5000ms}));
    ASSERT_TRUE(failure);
    try {
        std::rethrow_exception(failure);
    } catch (const std::runtime_error &error) {
        EXPECT_STREQ(error.what(), "first");
    }
}

TEST(JoinRetry, SuccessfulJoinReturnsNoFailureWithoutSleeping) {
    int sleeps = 0;
    EXPECT_FALSE(join_with_retry([] {}, [&](std::chrono::milliseconds) { ++sleeps; }));
    EXPECT_EQ(sleeps, 0);
}
