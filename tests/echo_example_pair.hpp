#pragma once

#include "../examples/echo/echo_peer.hpp"

namespace p2pdc_test {
[[nodiscard]] std::function<void()>
run_echo_example_pair(p2pdc::DiscoveryConfig config, p2pdc::Executor executor,
                      p2pdc::Scheduler scheduler, p2pdc::IceConfig ice, bool expect_turn,
                      bool reject_peer, std::function<void(bool, std::string)> completion);
} // namespace p2pdc_test
