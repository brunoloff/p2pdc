#pragma once

#include "p2pdc/signaling.h"

namespace p2pdc::detail {
inline constexpr std::size_t signaling_wire_limit = 32768;
inline constexpr std::size_t signaling_payload_limit =
    signaling_wire_limit - signaling_wire_overhead;
} // namespace p2pdc::detail
