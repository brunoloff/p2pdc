#include "wasm_discovery.hpp"

#include <emscripten.h>

#include <cmath>
#include <unordered_map>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
std::unordered_map<unsigned, std::shared_ptr<WasmDiscovery>> watchers;
unsigned next_id = 1;
std::shared_ptr<WasmDiscovery> find(unsigned id) {
    const auto entry = watchers.find(id);
    return entry == watchers.end() ? nullptr : entry->second;
}
std::uint64_t clock_value(double value) {
    constexpr double maximum = 9007199254740991.0 - 300.0;
    if (!std::isfinite(value) || value < 0 || value > maximum || std::floor(value) != value)
        throw std::invalid_argument("invalid discovery clock");
    return static_cast<std::uint64_t>(value);
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE unsigned p2pdc_discovery_create(const std::byte *config, unsigned size) {
    if (watchers.size() >= 16 || !next_id || !config || size != wasm_discovery_config_size)
        return 0;
    try {
        const auto id = next_id++;
        watchers.emplace(id, WasmDiscovery::create(id, ByteView(config, size)));
        return id;
    } catch (...) {
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE void p2pdc_discovery_destroy(unsigned id) { watchers.erase(id); }
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_healthy(unsigned id) {
    auto watch = find(id);
    return watch && watch->healthy();
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_key(unsigned id, std::byte *out, unsigned size) {
    auto watch = find(id);
    if (!watch || !out)
        return 0;
    try {
        return watch->key(std::span(out, size));
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_window_key(unsigned id, double now, std::byte *out,
                                                    unsigned size) {
    auto watch = find(id);
    if (!watch || !out)
        return 0;
    try {
        return watch->window_key(clock_value(now), std::span(out, size));
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_self(unsigned id, std::byte *out, unsigned size) {
    auto watch = find(id);
    if (!watch || !out)
        return 0;
    try {
        return watch->self(std::span(out, size));
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_publish(unsigned id, double now, std::byte *out,
                                                 unsigned size) {
    auto watch = find(id);
    if (!watch || !out)
        return 0;
    try {
        return watch->publish(clock_value(now), std::span(out, size));
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_publish_windowed(unsigned id, double now, std::byte *out,
                                                          unsigned size) {
    auto watch = find(id);
    if (!watch || !out)
        return 0;
    try {
        return watch->publish(clock_value(now), std::span(out, size), true);
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_receive(unsigned id, const std::byte *wire, unsigned size,
                                                 double now) {
    auto watch = find(id);
    if (!watch || !watch->healthy() || (!wire && size))
        return 2;
    try {
        const auto current = clock_value(now);
        if (size != advertisement_wire_size)
            return watch->sweep(current) ? 1 : 2;
        return watch->receive(ByteView(wire, size), current);
    } catch (...) {
        watchers.erase(id);
        return 2;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_sweep(unsigned id, double now) {
    auto watch = find(id);
    if (!watch)
        return 0;
    try {
        return watch->sweep(clock_value(now));
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_current(unsigned id, unsigned handle, double now,
                                                 std::byte *out, unsigned size) {
    auto watch = find(id);
    if (!watch || !out)
        return 0;
    try {
        return watch->current(handle, clock_value(now), std::span(out, size));
    } catch (...) {
        watchers.erase(id);
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_discovery_incoming(unsigned local_id, unsigned watch_id,
                                                  const std::byte *wire, unsigned size, double now,
                                                  int inspect_only, std::byte *out,
                                                  unsigned out_size) {
    auto local = find(local_id), watch = find(watch_id);
    if (!local || !watch || (!wire && size) || !out || size > 32768 || out_size != 20 ||
        (inspect_only != 0 && inspect_only != 1))
        return 2;
    try {
        return local->incoming(*watch, ByteView(wire, size), clock_value(now), inspect_only == 1,
                               std::span(out, out_size));
    } catch (...) {
        return 2;
    }
}
}
