#pragma once

#include "p2pdc/node.h"

namespace p2pdc_test {
enum class RaceMode { Collision, Cancel, Reject, CancelAccepted, Persistent };
[[nodiscard]] std::function<void()> run_node_race_consumer(p2pdc::DiscoveryConfig config,
                                                           p2pdc::Executor executor,
                                                           p2pdc::Scheduler scheduler,
                                                           RaceMode mode,
                                                           std::function<void(bool)> completion);
} // namespace p2pdc_test
