#include "../src/core/round_trip.hpp"
#include "../src/transport/connection_status.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using p2pdc::detail::RoundTripSmoother;

TEST(RoundTripSmoother, TakesTheFirstSampleAndSmoothsTheRestByAQuarter) {
    RoundTripSmoother smoother;
    EXPECT_FALSE(smoother.value());
    ASSERT_TRUE(smoother.sample(40ms));
    EXPECT_EQ(smoother.value(), 40ms);
    ASSERT_TRUE(smoother.sample(80ms));
    EXPECT_EQ(smoother.value(), 50ms); // 40 + (80 - 40) / 4
    ASSERT_TRUE(smoother.sample(std::chrono::microseconds{50400}));
    EXPECT_EQ(smoother.value(), 50ms); // rounded to whole milliseconds
}

TEST(RoundTripSmoother, IgnoresImpossibleSamples) {
    RoundTripSmoother smoother;
    EXPECT_FALSE(smoother.sample(-1ms));
    EXPECT_FALSE(smoother.sample(RoundTripSmoother::max_sample + 1ms));
    EXPECT_FALSE(smoother.value());
    ASSERT_TRUE(smoother.sample(0ms));
    EXPECT_EQ(smoother.value(), 0ms);
    EXPECT_FALSE(smoother.sample(10min));
    EXPECT_EQ(smoother.value(), 0ms);
}

TEST(RoundTripSmoother, IsDueOncePerIntervalAndForgetsOnReset) {
    RoundTripSmoother smoother;
    const auto start = std::chrono::steady_clock::time_point{} + 1h;
    EXPECT_TRUE(smoother.due(start));
    EXPECT_FALSE(smoother.due(start + RoundTripSmoother::sample_interval - 1ms));
    EXPECT_TRUE(smoother.due(start + RoundTripSmoother::sample_interval));
    ASSERT_TRUE(smoother.sample(20ms));
    smoother.reset();
    EXPECT_FALSE(smoother.value());
    EXPECT_TRUE(smoother.due(start + RoundTripSmoother::sample_interval + 1ms));
}

TEST(RoundTripSmoother, InvalidatedRoutesCarryNoRoundTrip) {
    p2pdc::RouteInfo route;
    route.round_trip_time = 25ms;
    EXPECT_FALSE(p2pdc::detail::invalidated_route(route).round_trip_time);
}

TEST(RoundTripSmoother, OwnersHearOfRouteChangesNotOfEveryObservation) {
    using p2pdc::CandidateKind;
    p2pdc::RouteInfo stored;
    p2pdc::RouteInfo next;
    next.current = p2pdc::RouteKind::Direct;
    next.selected_pair = p2pdc::CandidatePair{CandidateKind::Host, CandidateKind::Host};
    next.observed_at = std::chrono::steady_clock::now();
    EXPECT_TRUE(p2pdc::detail::update_route(stored, next));
    next.observed_at = *next.observed_at + 100ms;
    EXPECT_FALSE(p2pdc::detail::update_route(stored, next)) << "only observed again";
    EXPECT_EQ(stored.observed_at, next.observed_at) << "but the stored route is current";
    next.round_trip_time = 12ms;
    EXPECT_TRUE(p2pdc::detail::update_route(stored, next));
    next.selected_pair = p2pdc::CandidatePair{CandidateKind::Host, CandidateKind::Relayed};
    EXPECT_TRUE(p2pdc::detail::update_route(stored, next));
}
