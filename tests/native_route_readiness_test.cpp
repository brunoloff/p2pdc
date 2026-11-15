#include "../src/transport/native_config.hpp"

#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(NativeRouteReadiness, OnlyReportsSelectedPairWhileChannelIsUsable) {
    const CandidatePair pair{CandidateKind::Host, CandidateKind::Relayed};
    const auto ready = usable_route_pair(true, true, pair);
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->remote, CandidateKind::Relayed);
    EXPECT_FALSE(usable_route_pair(false, true, pair));
    EXPECT_FALSE(usable_route_pair(true, false, pair));
    EXPECT_FALSE(usable_route_pair(true, true, std::nullopt));
}

TEST(NativeRouteReadiness, SuspensionKeepsTurnHistoryWhileCurrentRouteBecomesUnknown) {
    RouteTracker tracker;
    const auto now = std::chrono::steady_clock::now();
    const CandidatePair pair{CandidateKind::Relayed, CandidateKind::Host};
    tracker.observe(usable_route_pair(true, true, pair), false, now);
    tracker.observe(usable_route_pair(true, false, pair), false, now + std::chrono::seconds(1));
    EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
    EXPECT_FALSE(tracker.route().selected_pair);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
    const CandidatePair direct{CandidateKind::Host, CandidateKind::Host};
    tracker.observe(usable_route_pair(true, true, direct), false, now + std::chrono::seconds(2));
    EXPECT_EQ(tracker.route().current, RouteKind::Direct);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
}
