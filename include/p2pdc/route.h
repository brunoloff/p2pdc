#pragma once

#include <chrono>
#include <optional>

namespace p2pdc {

enum class CandidateKind { Unknown, Host, ServerReflexive, PeerReflexive, Relayed };
// A peer-reflexive candidate may conceal a relay. Unless the other candidate
// is known to be relayed, such a pair is reported as Unknown.
enum class RouteKind { Unknown, Direct, TurnRelayed };
enum class TurnHistory { Unknown, NotUsed, Used };

struct CandidatePair {
    CandidateKind local;
    CandidateKind remote;
};

struct RouteInfo {
    RouteKind current = RouteKind::Unknown;
    TurnHistory turn_history = TurnHistory::Unknown;
    std::optional<CandidatePair> selected_pair;
    std::optional<std::chrono::steady_clock::time_point> observed_at;
};

// Only a complete, continuous observation can prove TURN was never used. The owner
// must invalidate on observation gaps, suspension, or expiration of route evidence.
class RouteTracker {
  public:
    void observe(std::optional<CandidatePair> pair, bool complete_coverage,
                 std::chrono::steady_clock::time_point observed_at);
    void invalidate(
        std::chrono::steady_clock::time_point invalidated_at = std::chrono::steady_clock::now());
    [[nodiscard]] RouteInfo route() const;

  private:
    RouteInfo info_;
    std::optional<std::chrono::steady_clock::time_point> invalidated_at_;
    bool coverage_complete_ = true;
    bool has_observation_ = false;
    bool turn_used_ = false;
};

} // namespace p2pdc
