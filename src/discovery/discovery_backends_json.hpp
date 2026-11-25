#pragma once

#include "p2pdc/discovery_backends.h"

#include <string>
#include <vector>

namespace p2pdc::detail {
// The JSON list that the browser bridge reads (src/browser/discovery_backends.mjs). Throws
// std::invalid_argument unless there are 1 to 8 backends and each is valid.
// Throws std::invalid_argument unless a native node accepts the list: at most
// max_discovery_backends entries, each valid.
void validate_discovery_backends(const std::vector<::p2pdc::DiscoveryBackend> &backends);
[[nodiscard]] std::string
discovery_backends_json(const std::vector<::p2pdc::DiscoveryBackend> &backends);
} // namespace p2pdc::detail
