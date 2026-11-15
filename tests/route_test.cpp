#include "p2pdc/route.h"

#include <gtest/gtest.h>

using namespace p2pdc;

TEST(RouteTracker, ClassifiesDirectAndUnknownPairs) {
    RouteTracker tracker;
    const auto now = std::chrono::steady_clock::now();
    EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Unknown);
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::ServerReflexive}, true, now);
    EXPECT_EQ(tracker.route().current, RouteKind::Direct);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::NotUsed);
    EXPECT_EQ(tracker.route().observed_at, now);
    tracker.observe(CandidatePair{CandidateKind::Unknown, CandidateKind::Host}, true, now);
    EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Unknown);
}

TEST(RouteTracker, RelayedEitherSideIsStickyAfterRouteChanges) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto pair : {CandidatePair{CandidateKind::Relayed, CandidateKind::Unknown},
                            CandidatePair{CandidateKind::Host, CandidateKind::Relayed}}) {
        RouteTracker tracker;
        tracker.observe(pair, false, now);
        EXPECT_EQ(tracker.route().current, RouteKind::TurnRelayed);
        EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
        tracker.invalidate(now);
        EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
        EXPECT_FALSE(tracker.route().selected_pair);
        EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
        tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, true,
                        now + std::chrono::seconds{1});
        EXPECT_EQ(tracker.route().current, RouteKind::Direct);
        EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
    }
}

TEST(RouteTracker, ObservationGapsCannotProveTurnWasNeverUsed) {
    RouteTracker tracker;
    const auto now = std::chrono::steady_clock::now();
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, false, now);
    EXPECT_EQ(tracker.route().current, RouteKind::Direct);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Unknown);
    tracker.observe(std::nullopt, true, now);
    EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, true, now);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Unknown);
}

TEST(RouteTracker, OlderObservationsCannotReplaceCurrentRoute) {
    RouteTracker tracker;
    const auto now = std::chrono::steady_clock::now();
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, false, now);
    tracker.observe(CandidatePair{CandidateKind::Relayed, CandidateKind::Host}, false,
                    now - std::chrono::seconds{1});
    EXPECT_EQ(tracker.route().current, RouteKind::Direct);
    EXPECT_EQ(tracker.route().observed_at, now);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
}

TEST(RouteTracker, InvalidationRejectsStaleAndEqualTimeObservations) {
    RouteTracker tracker;
    const auto now = std::chrono::steady_clock::now();
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, true, now);
    tracker.invalidate(now + std::chrono::seconds{2});
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, true,
                    now + std::chrono::seconds{2});
    EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
    tracker.observe(CandidatePair{CandidateKind::Relayed, CandidateKind::Host}, true,
                    now + std::chrono::seconds{1});
    EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
    tracker.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, true,
                    now + std::chrono::seconds{3});
    EXPECT_EQ(tracker.route().current, RouteKind::Direct);
    EXPECT_EQ(tracker.route().turn_history, TurnHistory::Used);
}

TEST(RouteTracker, PeerReflexiveCandidatesDoNotProveDirectRoute) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto pair : {CandidatePair{CandidateKind::Host, CandidateKind::PeerReflexive},
                            CandidatePair{CandidateKind::PeerReflexive, CandidateKind::Host}}) {
        RouteTracker tracker;
        tracker.observe(pair, true, now);
        EXPECT_EQ(tracker.route().current, RouteKind::Unknown);
        EXPECT_EQ(tracker.route().turn_history, TurnHistory::Unknown);
    }
    RouteTracker transition;
    transition.observe(CandidatePair{CandidateKind::Host, CandidateKind::PeerReflexive}, true, now);
    transition.observe(CandidatePair{CandidateKind::Host, CandidateKind::Host}, true,
                       now + std::chrono::seconds{1});
    EXPECT_EQ(transition.route().current, RouteKind::Direct);
    EXPECT_EQ(transition.route().turn_history, TurnHistory::Unknown);
    RouteTracker relayed;
    relayed.observe(CandidatePair{CandidateKind::Relayed, CandidateKind::PeerReflexive}, true, now);
    EXPECT_EQ(relayed.route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(relayed.route().turn_history, TurnHistory::Used);
}
