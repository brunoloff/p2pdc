#include "standalone_host.hpp"

#include <emscripten.h>

EM_JS(void, echo_report_result, (int status),
      { document.body.dataset.result = status == 0 ? "pass" : "fail"; });
namespace p2pdc_echo {
int host_wait(std::shared_future<int>) { return 0; }
void host_complete(int status) { echo_report_result(status); }
} // namespace p2pdc_echo
