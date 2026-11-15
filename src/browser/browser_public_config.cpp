#include "browser_public_config.hpp"
#include "../security/signaling_limits.hpp"

#include <utility>

namespace p2pdc::detail {
BrowserMailboxConfig make_browser_public_config(const ConnectionAdmission &admission,
                                                const IncomingOffer *offer, IceConfig ice,
                                                MemoryBudget budget, std::uint32_t message_limit,
                                                std::function<std::uint64_t()> now) {
    const auto role = offer ? HandshakeRole::Answerer : HandshakeRole::Offerer;
    const auto attempt = offer ? offer->envelope.fields.attempt_id : random_attempt_id();
    auto binding = make_attempt_binding(admission, budget, now, attempt, role);
    if (offer)
        verify_incoming_offer(admission, *offer, binding,
                              {now(), 60, admission.remote_clock_skew, signaling_payload_limit});
    BrowserSecureConfig secure{binding,
                               admission.local.identity->signing,
                               admission.local.identity->recipient,
                               admission.capability,
                               std::move(ice),
                               message_limit,
                               std::move(now),
                               admission.remote_clock_skew};
    return {admission.store,
            admission.local.fields.signaling_inbox,
            admission.remote.fields.signaling_inbox,
            std::move(secure),
            std::move(budget),
            offer ? std::optional<Bytes>(offer->wire) : std::nullopt};
}
} // namespace p2pdc::detail
