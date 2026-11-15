#pragma once

#include "p2pdc/node.h"

namespace p2pdc_test {
[[nodiscard]] bool rejected_ice_keeps_request(p2pdc::Node &node, p2pdc::IncomingListener &listener,
                                              p2pdc::Scheduler scheduler,
                                              p2pdc::ConnectionHandlers handlers);
} // namespace p2pdc_test
