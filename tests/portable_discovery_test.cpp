#include <gtest/gtest.h>

bool portable_discovery_smoke();

TEST(PortableDiscovery, PublishesWatchesAndReportsBackendFailure) {
    EXPECT_TRUE(portable_discovery_smoke());
}
