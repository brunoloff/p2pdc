#pragma once

#include "../discovery/record_store.hpp"
#include "p2pdc/discovery_backends.h"

#include <string>
#include <utility>
#include <vector>

namespace p2pdc::detail {
// Uses the discovery backends, all at once (src/browser/discovery_backends.mjs). Throws
// std::invalid_argument for invalid backends or limits.
// Methods and callbacks must run on the browser event-loop thread. The bridge
// is bundled into the WebAssembly target and is not an application API.
[[nodiscard]] RecordStore
make_browser_record_store(const std::vector<::p2pdc::DiscoveryBackend> &backends,
                          StoreLimits limits, unsigned timeout_ms = 10000);
// One OpenDHT proxy.
[[nodiscard]] inline RecordStore
make_browser_record_store(std::string proxy_url, StoreLimits limits, unsigned timeout_ms = 10000) {
    return make_browser_record_store({OpenDhtProxies{{std::move(proxy_url)}}}, limits, timeout_ms);
}
} // namespace p2pdc::detail
