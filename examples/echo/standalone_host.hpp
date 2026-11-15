#pragma once

#include <future>

namespace p2pdc_echo {
// Native hosts wait; browser hosts return to their event loop.
[[nodiscard]] int host_wait(std::shared_future<int> completion);
void host_complete(int status);
} // namespace p2pdc_echo
