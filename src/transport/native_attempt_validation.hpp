#pragma once

#include "native_attempt.hpp"

namespace p2pdc::detail {
inline bool valid_gate_limits(const NativeAttemptConfig &config) {
    return config.gate_transport_limit >= 1 + handshake_record_size &&
           config.gate_transport_limit <= config.channel_limits.frames.max_message_size &&
           config.application_limit <= config.gate_transport_limit - 1;
}
inline bool valid_accepted_offer(const NativeAttemptConfig &config) {
    if (!config.accepted_offer)
        return true;
    const auto size = config.accepted_offer->size();
    return config.binding.role == HandshakeRole::Answerer && size >= signaling_wire_overhead &&
           size - signaling_wire_overhead <= config.signaling_policy.max_payload_size;
}
} // namespace p2pdc::detail
