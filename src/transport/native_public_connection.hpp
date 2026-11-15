#pragma once

#include "../discovery/record_store.hpp"
#include "connection_backend.hpp"
#include "p2pdc/callback_gate.h"

namespace p2pdc::detail {

[[nodiscard]] inline ConnectError connection_store_error(StoreError error) {
    if (error == StoreError::ResourceLimit || error == StoreError::AllocationFailed)
        return ConnectError::ResourceLimit;
    if (error == StoreError::NotRunning || error == StoreError::PermanentFailure)
        return ConnectError::Signaling;
    return ConnectError::Backend;
}

void validate_public_connection_settings(const Executor &executor, const Scheduler &scheduler,
                                         const ConnectOptions &options,
                                         const ConnectionHandlers &handlers);
void validate_public_ice_settings(const IceConfig &ice);

// Allocates a dormant owner. The Node registers it before calling start().
[[nodiscard]] std::shared_ptr<ConnectionBackend>
make_public_connection(Executor executor, CallbackGate gate, Scheduler scheduler,
                       ConnectOptions options, ConnectionHandlers handlers,
                       std::function<bool()> revalidate);
} // namespace p2pdc::detail
