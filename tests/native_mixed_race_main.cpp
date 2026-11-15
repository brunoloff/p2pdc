#include "node_mixed_race.hpp"
#include "p2pdc/runtime.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace std::chrono_literals;

int main(int argc, char **argv) {
    if (argc != 3)
        throw std::invalid_argument("expected mode and role");
    const std::string_view mode = argv[1], role = argv[2];
    if ((mode != "collision" && mode != "reject" && mode != "cancel-accepted" &&
         mode != "cancel-dial") ||
        (role != "offerer" && role != "answerer"))
        throw std::invalid_argument("invalid mode or role");
    const auto selected = mode == "collision"         ? p2pdc_test::MixedRace::Collision
                          : mode == "reject"          ? p2pdc_test::MixedRace::Reject
                          : mode == "cancel-accepted" ? p2pdc_test::MixedRace::CancelAccepted
                                                      : p2pdc_test::MixedRace::CancelDial;
    p2pdc::Runtime runtime;
    auto executor = runtime.executor();
    auto scheduler = runtime.scheduler();
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    p2pdc::DiscoveryConfig config;
    config.network_id = 873198;
    config.bind_addresses = p2pdc::NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", 49411}};
    executor([config, executor, scheduler, selected, role, completion] {
        (void)p2pdc_test::run_mixed_race(config, executor, scheduler, selected, role == "offerer",
                                         [completion](bool ok) { completion->set_value(ok); });
    });
    std::cout << "READY" << std::endl;
    if (result.wait_for(40s) != std::future_status::ready || !result.get()) {
        std::cerr << "mixed native race failed\n";
        return 1;
    }
    std::cout << "PASS" << std::endl;
}
