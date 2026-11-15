#include "../src/discovery/announcement_budget.hpp"

#include <gtest/gtest.h>

using p2pdc::DiscoveryProtocol;
using p2pdc::detail::AnnouncementBudget;
using namespace std::chrono_literals;

TEST(AnnouncementBudgetTest, RollingLimitRecoversWithoutBoundaryBursts) {
    AnnouncementBudget budget;
    auto now = AnnouncementBudget::Clock::time_point{};
    for (std::size_t index = 0; index < AnnouncementBudget::capacity; ++index) {
        ASSERT_TRUE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
        now += 1s;
    }
    EXPECT_FALSE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    now = AnnouncementBudget::Clock::time_point{} + 300s;
    EXPECT_TRUE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    EXPECT_FALSE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    EXPECT_TRUE(budget.admit(now + 1s, DiscoveryProtocol::RotatingKeysV2));
}

TEST(AnnouncementBudgetTest, FixedKeyRetainsLifetimeLimitAndSharesBurstBudget) {
    AnnouncementBudget budget;
    auto now = AnnouncementBudget::Clock::time_point{};
    for (int index = 0; index < 8; ++index) {
        ASSERT_TRUE(budget.admit(now, DiscoveryProtocol::FixedKeyV1));
        ASSERT_TRUE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    }
    EXPECT_FALSE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    now += 300s;
    for (int index = 0; index < 8; ++index)
        ASSERT_TRUE(budget.admit(now, DiscoveryProtocol::FixedKeyV1));
    EXPECT_FALSE(budget.admit(now + 300s, DiscoveryProtocol::FixedKeyV1));
    EXPECT_TRUE(budget.admit(now + 300s, DiscoveryProtocol::RotatingKeysV2));
}

TEST(AnnouncementBudgetTest, RejectsInvalidProtocolAndClockRegression) {
    AnnouncementBudget budget;
    const auto now = AnnouncementBudget::Clock::time_point{} + 1s;
    // A fixed-underlying enum can represent this value; it is not a valid protocol.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid = static_cast<DiscoveryProtocol>(255);
    EXPECT_FALSE(budget.admit(now, invalid));
    ASSERT_TRUE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    EXPECT_FALSE(budget.admit(now - 1s, DiscoveryProtocol::RotatingKeysV2));
    EXPECT_TRUE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
}

TEST(AnnouncementBudgetTest, SaturatedClockDoesNotWrapTheInterval) {
    AnnouncementBudget budget;
    const auto now = AnnouncementBudget::Clock::time_point::max() - 1s;
    for (std::size_t index = 0; index < AnnouncementBudget::capacity; ++index)
        ASSERT_TRUE(budget.admit(now, DiscoveryProtocol::RotatingKeysV2));
    EXPECT_FALSE(budget.admit(AnnouncementBudget::Clock::time_point::max(),
                              DiscoveryProtocol::RotatingKeysV2));
}
