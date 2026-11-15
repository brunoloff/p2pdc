#include "p2pdc/discovery_key.h"

#include <gtest/gtest.h>

using namespace p2pdc;

TEST(DiscoveryKey, MatchesIndependentBlake2VectorAndRejectsInvalidLengths) {
    DiscoveryCapability capability{};
    for (std::size_t i = 0; i < capability.size(); ++i) {
        capability[i] = static_cast<std::byte>(i);
    }
    const DiscoveryKey expected{std::byte{0xea}, std::byte{0x76}, std::byte{0xa9}, std::byte{0xb4},
                                std::byte{0xad}, std::byte{0x1d}, std::byte{0xfb}, std::byte{0xe7},
                                std::byte{0x93}, std::byte{0xc5}, std::byte{0xea}, std::byte{0xe8},
                                std::byte{0xd9}, std::byte{0x6b}, std::byte{0xb5}, std::byte{0x1e},
                                std::byte{0x41}, std::byte{0xb6}, std::byte{0x57}, std::byte{0x32}};
    const auto derived = derive_discovery_key(capability);
    ASSERT_TRUE(std::holds_alternative<DiscoveryKey>(derived));
    EXPECT_EQ(std::get<DiscoveryKey>(derived), expected);
    EXPECT_EQ(std::get<CryptoError>(derive_discovery_key({})), CryptoError::InvalidLength);
    EXPECT_EQ(std::get<CryptoError>(derive_discovery_key(Bytes(capability_size + 1))),
              CryptoError::InvalidLength);
    capability[0] ^= std::byte{1};
    EXPECT_NE(std::get<DiscoveryKey>(derive_discovery_key(capability)), expected);
}

TEST(DiscoveryKey, GeneratesIndependentCapabilities) {
    const auto first = generate_discovery_capability();
    const auto second = generate_discovery_capability();
    ASSERT_TRUE(std::holds_alternative<DiscoveryCapability>(first));
    ASSERT_TRUE(std::holds_alternative<DiscoveryCapability>(second));
    EXPECT_NE(std::get<DiscoveryCapability>(first), std::get<DiscoveryCapability>(second));
}
