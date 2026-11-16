#pragma once

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>

namespace p2pdc::detail {

// Calls join until it succeeds and returns the first failure, if any. join must
// eventually succeed before the owner may release the runner's resources, so
// failures are retried rather than abandoned (returning early could leave
// backend threads using freed state). The interval starts at 100 ms and backs
// off to five seconds; the first failure is logged once.
template <class Join, class Sleep>
[[nodiscard]] std::exception_ptr join_with_retry(Join &&join, Sleep &&sleep) {
    std::exception_ptr first_failure;
    auto delay = std::chrono::milliseconds(100);
    for (;;) {
        try {
            join();
            return first_failure;
        } catch (const std::exception &error) {
            if (!first_failure) {
                first_failure = std::current_exception();
                std::cerr << "p2pdc: backend join failed; retrying: " << error.what() << '\n';
            }
        } catch (...) {
            if (!first_failure) {
                first_failure = std::current_exception();
                std::cerr << "p2pdc: backend join failed; retrying\n";
            }
        }
        sleep(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(5000));
    }
}

} // namespace p2pdc::detail
