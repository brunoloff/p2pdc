#include "p2pdc/route.h"

namespace p2pdc {
namespace {
RouteKind classify(CandidatePair pair) {
    if (pair.local == CandidateKind::Relayed || pair.remote == CandidateKind::Relayed) {
        return RouteKind::TurnRelayed;
    }
    if (pair.local == CandidateKind::Unknown || pair.remote == CandidateKind::Unknown ||
        pair.local == CandidateKind::PeerReflexive || pair.remote == CandidateKind::PeerReflexive) {
        return RouteKind::Unknown;
    }
    return RouteKind::Direct;
}
} // namespace

void RouteTracker::observe(std::optional<CandidatePair> pair, bool complete_coverage,
                           std::chrono::steady_clock::time_point observed_at) {
    const auto current = pair ? classify(*pair) : RouteKind::Unknown;
    turn_used_ = turn_used_ || current == RouteKind::TurnRelayed;
    if ((info_.observed_at && observed_at < *info_.observed_at) ||
        (invalidated_at_ && observed_at <= *invalidated_at_)) {
        return;
    }
    has_observation_ = true;
    info_.selected_pair = pair;
    info_.observed_at = observed_at;
    info_.current = current;
    coverage_complete_ =
        coverage_complete_ && complete_coverage && info_.current != RouteKind::Unknown;
}

void RouteTracker::invalidate(std::chrono::steady_clock::time_point invalidated_at) {
    info_.current = RouteKind::Unknown;
    info_.selected_pair.reset();
    coverage_complete_ = false;
    if (!invalidated_at_ || invalidated_at > *invalidated_at_) {
        invalidated_at_ = invalidated_at;
    }
    if (info_.observed_at && *info_.observed_at > *invalidated_at_) {
        invalidated_at_ = info_.observed_at;
    }
}

RouteInfo RouteTracker::route() const {
    auto result = info_;
    if (turn_used_) {
        result.turn_history = TurnHistory::Used;
    } else if (has_observation_ && coverage_complete_) {
        result.turn_history = TurnHistory::NotUsed;
    } else {
        result.turn_history = TurnHistory::Unknown;
    }
    return result;
}

} // namespace p2pdc
