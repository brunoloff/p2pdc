#include "echo_example_pair.hpp"
#include "node_connection_consumer.hpp"
#include "node_mixed_race.hpp"
#include "node_race_consumer.hpp"
#include "p2pdc/runtime.h"

#include <emscripten.h>

#include <iostream>
#include <memory>

void run_browser_native_public_peer(p2pdc::Runtime &, bool, bool, std::function<void(bool)>);

EM_JS(void, report_public_connection, (int success), {
    document.body.dataset.result = success ? "pass" : "fail";
    document.body.textContent =
        success ? "pass: public C++ connection"
                : "fail: public C++ connection " + (document.body.dataset.debug || "");
});
EM_JS(int, relay_mode, (), {
    const mode = new URLSearchParams(location.search).get("turn");
    return mode == "required"      ? 1
           : mode == "fallback"    ? 2
           : mode == "tls"         ? 3
           : mode == "tls-timed"   ? 4
           : mode == "tls-expired" ? 5
                                   : 0;
});
EM_JS(int, example_mode, (), {
    const mode = new URLSearchParams(location.search).get("example");
    return mode == "echo" ? 1 : mode == "reject" ? 2 : 0;
});
EM_JS(int, interop_mode, (), {
    const mode = new URLSearchParams(location.search).get("interop");
    return mode == "offerer" ? 1 : mode == "answerer" ? 2 : mode == "recovery" ? 3 : 0;
});
EM_JS(int, race_mode, (), {
    const mode = new URLSearchParams(location.search).get("race");
    return mode == "collision"         ? 1
           : mode == "cancel"          ? 2
           : mode == "reject"          ? 3
           : mode == "cancel-accepted" ? 4
           : mode == "persistent"      ? 5
                                       : 0;
});
EM_JS(int, mixed_mode, (), {
    const mode = new URLSearchParams(location.search).get("mixed");
    return mode == "collision"         ? 1
           : mode == "reject"          ? 2
           : mode == "cancel-accepted" ? 3
           : mode == "cancel-dial"     ? 4
                                       : 0;
});
EM_JS(int, mixed_offerer, (),
      { return new URLSearchParams(location.search).get("role") == "offerer"; });
EM_JS(int, unavailable_mode, (), {
    const mode = new URLSearchParams(location.search).get("unsupported");
    if (mode == "broken") {
        globalThis.RTCPeerConnection = function() { throw new Error("test backend failure"); };
        return 2;
    }
    if (mode != "1")
        return 0;
    globalThis.RTCPeerConnection = undefined;
    return 1;
});
EM_JS(int, unavailable_accept_mode, (), {
    const mode = new URLSearchParams(location.search).get("accept-unsupported");
    return mode == "broken" ? 2 : mode == "1" ? 1 : 0;
});

int main() {
    try {
        auto runtime = std::make_shared<p2pdc::Runtime>();
        const int interop = interop_mode();
        if (interop) {
            run_browser_native_public_peer(*runtime, interop == 1 || interop == 3, interop == 3,
                                           [runtime](bool ok) { report_public_connection(ok); });
            return 0;
        }
        p2pdc::DiscoveryConfig config;
        config.browser_backends = {p2pdc::OpenDhtProxies{{"http://127.0.0.1:49412"}}};
        const int mixed = mixed_mode();
        if (mixed) {
            (void)p2pdc_test::run_mixed_race(
                std::move(config), runtime->executor(), runtime->scheduler(),
                static_cast<p2pdc_test::MixedRace>(mixed - 1), mixed_offerer(),
                [runtime](bool ok) { report_public_connection(ok); });
            return 0;
        }
        const int race = race_mode();
        if (race) {
            (void)p2pdc_test::run_node_race_consumer(
                std::move(config), runtime->executor(), runtime->scheduler(),
                static_cast<p2pdc_test::RaceMode>(race - 1),
                [runtime](bool ok) { report_public_connection(ok); });
            return 0;
        }
        const int relay = relay_mode();
        p2pdc::IceConfig ice;
        if (relay) {
            const bool tls = relay >= 3;
            const std::string user = relay == 4   ? "2000000000:p2pdc"
                                     : relay == 5 ? "1:p2pdc"
                                                  : "p2pdc";
            const std::string password = relay == 4   ? "VPRtuSrVPlp9LTjU4EoAof3yRB0="
                                         : relay == 5 ? "8zsChuzHi08+WyOX+JXi2hVp9EA="
                                                      : "temporary-test-password";
            ice.servers = {p2pdc::TurnServer{
                tls ? "turn.test" : "10.42.0.1", static_cast<std::uint16_t>(tls ? 53490 : 34789),
                user, password, tls ? p2pdc::TurnTransport::Tls : p2pdc::TurnTransport::Tcp}};
            if (relay == 1 || tls)
                ice.relay_policy = p2pdc::RelayPolicy::RequireLocalRelay;
        }
        const int example = example_mode();
        if (example) {
            (void)p2pdc_test::run_echo_example_pair(
                std::move(config), runtime->executor(), runtime->scheduler(), std::move(ice),
                relay != 0, example == 2, [runtime, example](bool ok, std::string reason) {
                    report_public_connection(
                        example == 1
                            ? ok
                            : !ok &&
                                  reason == "incoming peer is not authorized or no longer current");
                });
            return 0;
        }
        (void)p2pdc_test::run_node_connection_consumer(
            std::move(config), runtime->executor(), runtime->scheduler(), std::move(ice), relay,
            [runtime](bool ok) { report_public_connection(ok); }, unavailable_mode(),
            unavailable_accept_mode());
    } catch (const std::exception &error) {
        std::cerr << "public connection smoke: " << error.what() << '\n';
        report_public_connection(0);
    }
}
