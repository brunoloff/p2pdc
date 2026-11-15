#include "wasm_secure_attempt.hpp"

#include <emscripten.h>

#include <cmath>
#include <limits>
#include <unordered_map>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
std::unordered_map<unsigned, std::shared_ptr<WasmSecureAttempt>> attempts;
unsigned next_id = 1;
std::shared_ptr<WasmSecureAttempt> find(unsigned id) {
    const auto entry = attempts.find(id);
    return entry == attempts.end() ? nullptr : entry->second;
}
SignalingPolicy policy(double time) {
    constexpr double maximum = 9007199254740991.0 - 60.0;
    if (!std::isfinite(time) || time < 0 || time > maximum || std::floor(time) != time)
        throw std::invalid_argument("invalid signaling clock");
    return {static_cast<std::uint64_t>(time), 60, 5, 32768 - signaling_wire_overhead};
}
template <class F> int invoke(unsigned id, F action) {
    auto owner = find(id);
    if (!owner || !owner->active())
        return static_cast<int>(FlowResult::Closed);
    try {
        return action(*owner);
    } catch (...) {
        owner->fail(AttemptFailure::Resource);
        return static_cast<int>(FlowResult::Failed);
    }
}
ByteView bytes(const std::byte *data, unsigned size, unsigned maximum) {
    if ((!data && size) || size > maximum)
        throw std::invalid_argument("invalid byte input");
    return ByteView(data, size);
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE unsigned p2pdc_secure_create(const std::byte *config, unsigned size) {
    if (attempts.size() >= 16 || next_id == 0 || !config || size != wasm_attempt_config_size)
        return 0;
    try {
        const unsigned id = next_id++;
        auto owner = WasmSecureAttempt::create(id, ByteView(config, size));
        attempts.emplace(id, std::move(owner));
        return id;
    } catch (...) {
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE void p2pdc_secure_destroy(unsigned id) {
    auto owner = find(id);
    if (owner) {
        owner->close();
        attempts.erase(id);
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_description(unsigned id, const std::byte *data, unsigned size,
                                                  double time) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        auto now = policy(time);
        return static_cast<int>(owner.attempt().local_description(bytes(data, size, 32768), now.now,
                                                                  now.now + 60, now));
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_candidate(unsigned id, const char *candidate,
                                                unsigned candidate_size, const char *mid,
                                                unsigned mid_size, double time) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        auto text = bytes(reinterpret_cast<const std::byte *>(candidate), candidate_size, 4096);
        auto media = bytes(reinterpret_cast<const std::byte *>(mid), mid_size, 256);
        if (text.empty() || media.empty())
            throw std::invalid_argument("candidate requires mid");
        auto now = policy(time);
        CandidateView value{{reinterpret_cast<const char *>(text.data()), text.size()},
                            {reinterpret_cast<const char *>(media.data()), media.size()}};
        return static_cast<int>(owner.attempt().local_candidate(value, now.now, now.now + 60, now));
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_receive(unsigned id, const std::byte *data, unsigned size,
                                              double time) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        return static_cast<int>(
            owner.attempt().receive_signaling(bytes(data, size, 32768), policy(time)));
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_description_done(unsigned id, int success) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        return static_cast<int>(owner.attempt().description_applied(success != 0));
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_candidate_done(unsigned id, unsigned operation, int success) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        return static_cast<int>(owner.attempt().candidate_applied(operation, success != 0));
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_ready(unsigned id, int ready) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        owner.attempt().set_transport_ready(ready != 0);
        return owner.active() ? 0 : 2;
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_frame(unsigned id, const std::byte *data, unsigned size) {
    return invoke(id, [=](WasmSecureAttempt &owner) {
        owner.attempt().on_frame(bytes(data, size, 1024));
        return owner.active() ? 0 : 2;
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_send(unsigned id, const std::byte *data, unsigned size) {
    auto owner = find(id);
    if (!owner || !owner->active())
        return static_cast<int>(ChannelSend::Closed);
    try {
        const auto result = owner->attempt().send(bytes(data, size, 4 * 1024 * 1024));
        return static_cast<int>(owner->active() ? result : ChannelSend::Failed);
    } catch (...) {
        owner->fail(AttemptFailure::Resource);
        return static_cast<int>(ChannelSend::Failed);
    }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_wake(unsigned id) {
    return invoke(id, [](WasmSecureAttempt &owner) {
        owner.attempt().wake();
        return owner.active() ? 0 : 2;
    });
}
EMSCRIPTEN_KEEPALIVE int p2pdc_secure_timeout(unsigned id) {
    return invoke(id, [](WasmSecureAttempt &owner) {
        owner.attempt().timeout();
        return owner.active() ? 0 : 2;
    });
}
}
