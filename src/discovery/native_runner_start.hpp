#pragma once

#include "native_runner_owner.hpp"
#include "p2pdc/discovery_config.h"

namespace p2pdc::detail {

// Validates before returning the startup function. No network work occurs until
// the function runs on the runner owner worker. DNS and bootstrap are async.
[[nodiscard]] RunnerStart make_native_runner_start(NativeDiscoveryConfig config);

} // namespace p2pdc::detail
