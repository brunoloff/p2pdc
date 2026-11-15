#include "../src/discovery/discovery_window.hpp"

#include <gtest/gtest.h>

#include <limits>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(DiscoveryWindow, MatchesIndependentKeyVectorAndSeparatesWindows) {
    DiscoveryCapability capability{};
    for (std::size_t i = 0; i < capability.size(); ++i)
        capability[i] = static_cast<std::byte>(i);
    const DiscoveryKey expected{std::byte{0x11}, std::byte{0xc2}, std::byte{0x60}, std::byte{0xc6},
                                std::byte{0x03}, std::byte{0x63}, std::byte{0x58}, std::byte{0xb9},
                                std::byte{0xa0}, std::byte{0x9c}, std::byte{0x07}, std::byte{0x04},
                                std::byte{0x21}, std::byte{0x49}, std::byte{0xd9}, std::byte{0xda},
                                std::byte{0x73}, std::byte{0xd1}, std::byte{0xf9}, std::byte{0x53}};
    const auto key = derive_discovery_window_key(capability, 0);
    ASSERT_TRUE(std::holds_alternative<DiscoveryKey>(key));
    EXPECT_EQ(std::get<DiscoveryKey>(key), expected);
    const DiscoveryKey later{std::byte{0xba}, std::byte{0xb9}, std::byte{0x27}, std::byte{0x18},
                             std::byte{0xc5}, std::byte{0x1c}, std::byte{0x00}, std::byte{0xd6},
                             std::byte{0xb6}, std::byte{0xd9}, std::byte{0x2e}, std::byte{0xb5},
                             std::byte{0x29}, std::byte{0x10}, std::byte{0xff}, std::byte{0xc0},
                             std::byte{0xcc}, std::byte{0x9a}, std::byte{0x56}, std::byte{0x18}};
    EXPECT_EQ(std::get<DiscoveryKey>(derive_discovery_window_key(capability, 123456789)), later);
    EXPECT_NE(std::get<DiscoveryKey>(derive_discovery_window_key(capability, 1)), expected);
    EXPECT_NE(std::get<DiscoveryKey>(derive_discovery_key(capability)), expected);
    EXPECT_EQ(std::get<CryptoError>(derive_discovery_window_key({}, 0)),
              CryptoError::InvalidLength);
    capability[0] ^= std::byte{1};
    EXPECT_NE(std::get<DiscoveryKey>(derive_discovery_window_key(capability, 0)), expected);
}

TEST(DiscoveryWindow, CoversValidIssueTimesAcrossBoundaries) {
    EXPECT_EQ(discovery_window_for(119), 0U);
    EXPECT_EQ(discovery_window_for(120), 1U);
    EXPECT_EQ(std::get<DiscoveryWindowRange>(discovery_window_range(0, 300, 30)),
              (DiscoveryWindowRange{0, 0}));
    for (std::uint64_t now = 0; now < 1000; ++now) {
        const auto result = discovery_window_range(now, 300, 30);
        ASSERT_TRUE(std::holds_alternative<DiscoveryWindowRange>(result));
        const auto range = std::get<DiscoveryWindowRange>(result);
        EXPECT_LE(range.last - range.first + 1, 4U);
        const auto lower = now > 330 ? now - 330 : 0;
        EXPECT_LE(range.first, discovery_window_for(lower));
        EXPECT_LE(discovery_window_for(now + 30), range.last);
    }
}

TEST(DiscoveryWindow, HandlesArithmeticLimitsAndRejectsUnboundedPolicies) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto result = discovery_window_range(maximum, 300, 30);
    ASSERT_TRUE(std::holds_alternative<DiscoveryWindowRange>(result));
    const auto range = std::get<DiscoveryWindowRange>(result);
    EXPECT_EQ(range.last, discovery_window_for(maximum));
    EXPECT_LE(range.last - range.first + 1, max_discovery_windows);
    EXPECT_EQ(std::get<DiscoveryWindowError>(discovery_window_range(100, 0, 0)),
              DiscoveryWindowError::InvalidLifetime);
    EXPECT_EQ(std::get<DiscoveryWindowError>(discovery_window_range(1000, maximum, maximum)),
              DiscoveryWindowError::TooManyWindows);
}
