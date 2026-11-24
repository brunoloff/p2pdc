#pragma once

#include "p2pdc/browser_backends.h"

#include <string>
#include <vector>

namespace p2pdc::detail {
// The JSON list that the browser bridge reads (src/browser/discovery_backends.mjs). Throws
// std::invalid_argument unless there are 1 to 8 backends and each is valid.
[[nodiscard]] std::string browser_backends_json(const std::vector<BrowserBackend> &backends);
} // namespace p2pdc::detail
