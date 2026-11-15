#include "native_incoming_config.hpp"

#include <utility>

namespace p2pdc::detail {
NativeAttemptConfig make_native_incoming_config(const ConnectionAdmission &admission,
                                                const IncomingOffer &offer, IceConfig ice,
                                                MemoryBudget budget,
                                                std::function<std::uint64_t()> now) {
    auto config =
        make_native_attempt_config(admission, std::move(ice), std::move(budget), now,
                                   offer.envelope.fields.attempt_id, HandshakeRole::Answerer);
    auto policy = config.signaling_policy;
    policy.now = now();
    const auto &binding = config.binding;
    verify_incoming_offer(admission, offer,
                          {binding.local, binding.remote, binding.local_box, binding.remote_box,
                           binding.attempt, binding.role},
                          policy);
    config.accepted_offer = offer.wire;
    return config;
}
} // namespace p2pdc::detail
