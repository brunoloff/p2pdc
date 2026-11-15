#include "standalone_host.hpp"

namespace p2pdc_echo {
int host_wait(std::shared_future<int> completion) { return completion.get(); }
void host_complete(int) {}
} // namespace p2pdc_echo
