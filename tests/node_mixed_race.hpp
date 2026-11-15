#pragma once

#include "p2pdc/node.h"

namespace p2pdc_test {
enum class MixedRace { Collision, Reject, CancelAccepted, CancelDial };
[[nodiscard]] std::function<void()>
run_mixed_race(p2pdc::DiscoveryConfig config, p2pdc::Executor executor, p2pdc::Scheduler scheduler,
               MixedRace mode, bool offerer, std::function<void(bool)> completion);
} // namespace p2pdc_test
