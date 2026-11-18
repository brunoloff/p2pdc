#pragma once

#include "../protocol/reliable_channel.hpp"
#include "p2pdc/connection.h"

namespace p2pdc::detail {

[[nodiscard]] inline RouteInfo invalidated_route(RouteInfo route) {
    route.current = RouteKind::Unknown;
    route.selected_pair.reset();
    route.observed_at.reset();
    if (route.turn_history != TurnHistory::Used) {
        route.turn_history = TurnHistory::Unknown;
    }
    return route;
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
