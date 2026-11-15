#pragma once

#include "native_public_watch.hpp"
#include "p2pdc/node.h"
#include "peer_admission.hpp"

#ifdef __EMSCRIPTEN__
#include "record_store.hpp"
#else
#include <opendht.h>
#endif

namespace p2pdc::detail {

// Internal, immutable-at-admission data. A future attempt owner must revalidate
// and track dependent handles, then cancel/close attempts before Node shutdown.
// Retaining a runner pointer does not mean the runner is still running.
#ifdef __EMSCRIPTEN__
using AdmissionContext = RecordStore;
#else
using AdmissionContext = std::shared_ptr<dht::DhtRunner>;
#endif

struct ConnectionAdmission {
#ifdef __EMSCRIPTEN__
    AdmissionContext store;
#else
    AdmissionContext runner;
#endif
    AnnouncementEndpoint local;
    Advertisement remote;
    std::shared_ptr<const Bytes> capability;
    std::uint64_t remote_max_lifetime;
    std::uint64_t remote_clock_skew;
};

using AdmissionResult = std::variant<ConnectionAdmission, AdmissionError>;

// Caller must first verify both backend objects are owned by the running Node.
[[nodiscard]] AdmissionResult resolve_owned_context(AdmissionContext context,
                                                    DiscoveryBackend &lease,
                                                    DiscoveryBackend &subscription,
                                                    const Advertisement &peer);

struct ConnectionAccess {
    // Owner executor only; no user callbacks or yield during validation.
    [[nodiscard]] static AdmissionResult resolve(Node &node, const Lease &lease,
                                                 const Subscription &subscription,
                                                 const Advertisement &peer);
};
} // namespace p2pdc::detail
