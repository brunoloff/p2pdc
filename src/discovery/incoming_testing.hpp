#pragma once

#include "incoming_backend.hpp"

namespace p2pdc::detail {

// Internal test seam: exposes a listener's backend so tests can drive paths
// that are hard to reach through the public API. Owner executor only.
struct IncomingTestingAccess {
    [[nodiscard]] static IncomingBackend &backend(IncomingListener &listener) {
        return *listener.state_;
    }
};

} // namespace p2pdc::detail
