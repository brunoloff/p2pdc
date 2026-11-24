#pragma once

#include "../discovery/record_store.hpp"

#include <string>
#include <utility>
#include <vector>

namespace p2pdc::detail {
// Uses HTTPS OpenDHT proxies (HTTP loopback is allowed for local tests), the first that
// works, in order (failover_proxy.mjs): 1 to 8 URLs.
// Methods and callbacks must run on the browser event-loop thread. The bridge
// is bundled into the WebAssembly target and is not an application API.
[[nodiscard]] RecordStore make_browser_record_store(const std::vector<std::string> &proxy_urls,
                                                    StoreLimits limits,
                                                    unsigned timeout_ms = 10000);
// One proxy.
[[nodiscard]] inline RecordStore
make_browser_record_store(std::string proxy_url, StoreLimits limits, unsigned timeout_ms = 10000) {
    return make_browser_record_store(std::vector<std::string>{std::move(proxy_url)}, limits,
                                     timeout_ms);
}
} // namespace p2pdc::detail
