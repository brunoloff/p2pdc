#include "p2pdc/runtime.h"

#include <emscripten.h>
#include <functional>
#include <iostream>
#include <memory>

std::function<void()> run_browser_transport_smoke(p2pdc::Runtime &, bool,
                                                  std::function<void(bool)>);
std::function<void()> run_browser_secure_attempt_smoke(p2pdc::Runtime &, bool,
                                                       std::function<void(bool)>);
EM_JS(int, test_relay, (), { return new URLSearchParams(location.search).get("turn") == "1"; });
EM_JS(void, report_transport, (int success), {
    document.body.dataset.result = success ? "pass" : "fail";
    document.body.textContent = success ? "pass: C++ secure WebRTC" : "fail: C++ secure WebRTC";
});
int main() {
    try {
        auto runtime = std::make_shared<p2pdc::Runtime>();
        const bool relay = test_relay();
        (void)run_browser_transport_smoke(*runtime, relay, [runtime, relay](bool ok) {
            if (!ok) {
                report_transport(0);
                return;
            }
            (void)run_browser_secure_attempt_smoke(
                *runtime, relay, [runtime](bool secure) { report_transport(secure); });
        });
    } catch (const std::exception &error) {
        std::cerr << "browser transport smoke: " << error.what() << '\n';
        report_transport(0);
    }
}
