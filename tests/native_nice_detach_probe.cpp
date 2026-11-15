#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <rtc/global.hpp>

namespace {
using Receive = void (*)(void *, unsigned, unsigned, unsigned, char *, void *);
using ReceiveEx = void (*)(void *, unsigned, unsigned, unsigned, char *, void *, void *);
using OldAttach = int (*)(void *, unsigned, unsigned, void *, Receive, void *);
using NewAttach = int (*)(void *, unsigned, unsigned, void *, ReceiveEx, void *, void (*)(void *));
std::atomic_uint safe_detaches{0}, legacy_detaches{0};

template <typename Function> Function original(const char *name) {
    auto symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        std::fprintf(stderr, "detach probe: cannot find %s\n", name);
        std::_Exit(90);
    }
    return std::bit_cast<Function>(symbol);
}
} // namespace

extern "C" int nice_agent_attach_recv(void *agent, unsigned stream, unsigned component,
                                      void *context, Receive callback, void *data) {
    if (!callback)
        ++legacy_detaches;
    return original<OldAttach>("nice_agent_attach_recv")(agent, stream, component, context,
                                                         callback, data);
}
extern "C" int nice_agent_attach_recv_ex(void *agent, unsigned stream, unsigned component,
                                         void *context, ReceiveEx callback, void *data,
                                         void (*destroy)(void *)) {
    if (!callback)
        ++safe_detaches;
    return original<NewAttach>("nice_agent_attach_recv_ex")(agent, stream, component, context,
                                                            callback, data, destroy);
}
__attribute__((destructor)) static void verify_detachment() {
    if ((rtc::P2pdcBackendFeatures() & 2U) == 0)
        std::_Exit(77);
    const auto safe = safe_detaches.load(), legacy = legacy_detaches.load();
    std::fprintf(stderr, "detach probe: safe=%u legacy=%u\n", safe, legacy);
    if (safe < 2 || legacy != 0)
        std::_Exit(91);
}
