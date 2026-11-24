#pragma once

#include "../protocol/reliable_channel.hpp"
#include "p2pdc/connection.h"

#include <optional>
#include <utility>

namespace p2pdc::detail {

[[nodiscard]] inline RouteInfo invalidated_route(RouteInfo route) {
    route.current = RouteKind::Unknown;
    route.selected_pair.reset();
    route.observed_at.reset();
    route.round_trip_time.reset();
    if (route.turn_history != TurnHistory::Used) {
        route.turn_history = TurnHistory::Unknown;
    }
    return route;
}

// Stores next as the connection's route; true when it differs in more than when it was
// observed, so owners hear of changes (round-trip time included) rather than every poll.
[[nodiscard]] inline bool update_route(RouteInfo &stored, const RouteInfo &next) {
    const auto pair = [](const std::optional<CandidatePair> &value) {
        return value ? std::optional{std::pair{value->local, value->remote}} : std::nullopt;
    };
    const bool changed = stored.current != next.current ||
                         stored.turn_history != next.turn_history ||
                         pair(stored.selected_pair) != pair(next.selected_pair) ||
                         stored.round_trip_time != next.round_trip_time;
    stored = next;
    return changed;
}

[[nodiscard]] inline SendStatus public_send_status(ChannelSend result) {
    switch (result) {
    case ChannelSend::Accepted:
        return SendStatus::Accepted;
    case ChannelSend::NotReady:
        return SendStatus::NotReady;
    case ChannelSend::TooLarge:
        return SendStatus::TooLarge;
    case ChannelSend::Backpressure:
        return SendStatus::Backpressure;
    case ChannelSend::Failed:
        return SendStatus::Failed;
    case ChannelSend::Closed:
        return SendStatus::Closed;
    }
    return SendStatus::Failed;
}

} // namespace p2pdc::detail
