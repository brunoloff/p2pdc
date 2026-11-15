#pragma once

#include "discovery_backend.hpp"
#include "record_store.hpp"
#include "p2pdc/callback_gate.h"
#include "p2pdc/discovery.h"
#include "p2pdc/memory_budget.h"

namespace p2pdc::detail {
using DiscoveryBackendResult = std::variant<std::shared_ptr<DiscoveryBackend>, WatchError>;
[[nodiscard]] DiscoveryBackendResult
make_public_watch(RecordStore store, DiscoveryCapability capability,
                  Scheduler scheduler, WatchOptions options, DiscoveryHandlers handlers,
                  Executor executor, CallbackGate gate, MemoryBudget budget);
using LeaseBackendResult = std::variant<std::shared_ptr<DiscoveryBackend>, AnnouncementError>;
void validate_watch(const Scheduler &scheduler, const WatchOptions &options,
                    const DiscoveryHandlers &handlers);
void validate_announcement(const Scheduler &scheduler, const AnnounceOptions &options,
                           const AnnounceHandlers &handlers);
[[nodiscard]] LeaseBackendResult make_public_lease(RecordStore store,
                                                   AnnouncementEndpoint endpoint,
                                                   DiscoveryCapability capability,
                                                   Scheduler scheduler, AnnounceOptions options,
                                                   AnnounceHandlers handlers, Executor executor,
                                                   CallbackGate gate, MemoryBudget budget);
} // namespace p2pdc::detail
