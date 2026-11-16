#include "../src/core/rate_window.hpp"

#include <gtest/gtest.h>

using namespace p2pdc::detail;
using namespace std::chrono_literals;

TEST(RateWindow, AdmitsUpToTheLimitPerWindow) {
    RateWindow window(2, 10s);
    const RateWindow::Clock::time_point start{};
    EXPECT_TRUE(window.admit(start));
    EXPECT_TRUE(window.admit(start + 1s));
    EXPECT_FALSE(window.admit(start + 9s));
    EXPECT_TRUE(window.admit(start + 10s));
    EXPECT_TRUE(window.admit(start + 11s));
    EXPECT_FALSE(window.admit(start + 12s));
}

TEST(RateWindow, RejectsInvalidSettings) {
    EXPECT_THROW(RateWindow(0, 1s), std::invalid_argument);
    EXPECT_THROW(RateWindow(1, 0s), std::invalid_argument);
}
