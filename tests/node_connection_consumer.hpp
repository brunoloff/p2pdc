#pragma once

#include "p2pdc/node.h"

namespace p2pdc_test {
// The identical consumer is compiled for native and WebAssembly integration tests.
[[nodiscard]] std::function<void()>
run_node_connection_consumer(p2pdc::DiscoveryConfig config, p2pdc::Executor executor,
                             p2pdc::Scheduler scheduler, p2pdc::IceConfig ice, bool expect_turn,
                             std::function<void(bool)> completion, int expected_backend_error = 0,
                             int test_accept_unsupported = 0);
} // namespace p2pdc_test
