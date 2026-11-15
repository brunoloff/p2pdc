#include "node_ice_validation_smoke.hpp"

#include <stdexcept>

namespace p2pdc_test {
bool rejected_ice_keeps_request(p2pdc::Node &node, p2pdc::IncomingListener &listener,
                                p2pdc::Scheduler scheduler, p2pdc::ConnectionHandlers handlers) {
    p2pdc::IceConfig invalid;
    invalid.servers = {p2pdc::StunServer{"", 3478}};
    try {
        (void)node.accept(listener, invalid, std::move(scheduler), {}, std::move(handlers));
        return false;
    } catch (const std::invalid_argument &) {
        return listener.state() == p2pdc::IncomingState::Pending;
    }
}
} // namespace p2pdc_test
