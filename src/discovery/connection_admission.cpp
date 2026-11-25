#include "connection_admission.hpp"

#include <utility>

namespace p2pdc::detail {
AdmissionResult resolve_owned_context(AdmissionContext context, DiscoveryBackend &lease,
                                      DiscoveryBackend &subscription, const Advertisement &peer) {
#ifndef __EMSCRIPTEN__
    if (!context.runner || !context.runner->isRunning()) {
        return AdmissionError::Closed;
    }
#endif
    auto resolved = resolve_peer_context(lease, subscription, peer);
    if (auto *error = std::get_if<AdmissionError>(&resolved))
        return *error;
    auto result = std::get<PeerAdmission>(std::move(resolved));
#ifdef __EMSCRIPTEN__
    return ConnectionAdmission{std::move(context),         std::move(result.local),
                               std::move(result.remote),   std::move(result.capability),
                               result.remote_max_lifetime, result.remote_clock_skew};
#else
    return ConnectionAdmission{std::move(context.runner),  std::move(result.local),
                               std::move(result.remote),   std::move(result.capability),
                               result.remote_max_lifetime, result.remote_clock_skew,
                               std::move(context.backends)};
#endif
}
} // namespace p2pdc::detail
