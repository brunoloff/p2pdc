#pragma once

#include "p2pdc/ice_config.h"

namespace p2pdc::detail {
struct BrowserIceServer {
    std::string url;
    std::string username;
    std::string password;
};
// Rejects malformed input before it is passed to a browser API. Credentials
// are copied and never logged; the caller owns their lifetime.
[[nodiscard]] std::vector<BrowserIceServer> browser_ice_servers(const IceConfig &config);
} // namespace p2pdc::detail
