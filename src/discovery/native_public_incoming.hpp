#pragma once

#include "incoming_backend.hpp"
#include "native_incoming_listener.hpp"

namespace p2pdc::detail {
[[nodiscard]] inline IncomingError incoming_error(IncomingListenerError error) {
    if (error == IncomingListenerError::ResourceLimit)
        return IncomingError::ResourceLimit;
    if (error == IncomingListenerError::Callback)
        return IncomingError::Callback;
    return IncomingError::Backend;
}
[[nodiscard]] inline IncomingError incoming_error(StoreError error) {
    if (error == StoreError::ResourceLimit || error == StoreError::AllocationFailed)
        return IncomingError::ResourceLimit;
    return IncomingError::Backend;
}
// Dormant until start(); Node registers and reserves its lease first.
[[nodiscard]] std::shared_ptr<IncomingBackend>
make_public_incoming(RecordStore store, IncomingContext initial,
                     std::function<std::optional<IncomingContext>()> context,
                     std::weak_ptr<DiscoveryBackend> lease,
                     std::weak_ptr<DiscoveryBackend> subscription, Executor executor,
                     CallbackGate gate, Scheduler scheduler, IncomingOptions options,
                     IncomingHandlers handlers, std::shared_ptr<IncomingOfferReplay> replay);
} // namespace p2pdc::detail
