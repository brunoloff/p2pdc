#pragma once

#include "../discovery/record_store.hpp"

#include <string>

namespace p2pdc::detail {
// Uses an HTTPS OpenDHT proxy (HTTP loopback is allowed for local tests).
// Methods and callbacks must run on the browser event-loop thread. The bridge
// is bundled into the WebAssembly target and is not an application API.
[[nodiscard]] RecordStore make_browser_record_store(std::string proxy_url, StoreLimits limits,
                                                    unsigned timeout_ms = 10000);
} // namespace p2pdc::detail
