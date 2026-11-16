#include "browser_transport.hpp"
#include "browser_ice_config.hpp"

#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

EM_JS(int, rtc_available, (), { return typeof globalThis.RTCPeerConnection == "function"; });

EM_JS(unsigned, rtc_begin,
      (unsigned role, unsigned relay, unsigned message, unsigned buffer, unsigned inbound,
       unsigned count),
      {
          try {
              return Module.p2pdcWebRtcBridge.begin(role ? "answerer" : "offerer",
                                                    relay ? "requireLocalRelay" : "automatic", {
                                                        maxMessageBytes : message,
                                                        maxBufferBytes : buffer,
                                                        maxInboundBytes : inbound,
                                                        maxInboundMessages : count
                                                    });
          } catch {
              return 0;
          }
      });
EM_JS(int, rtc_server,
      (unsigned id, const char *url, unsigned url_len, const char *user, unsigned user_len,
       const char *pass, unsigned pass_len),
      {
          try {
              Module.p2pdcWebRtcBridge.addServer(id, UTF8ToString(url, url_len),
                                                 UTF8ToString(user, user_len),
                                                 UTF8ToString(pass, pass_len));
              return 1;
          } catch {
              return 0;
          }
      });
EM_JS(int, rtc_open, (unsigned id), {
    try {
        Module.p2pdcWebRtcBridge.open(id);
        return 1;
    } catch {
        return 0;
    }
});
EM_JS(int, rtc_start, (unsigned id), {
    try {
        Module.p2pdcWebRtcBridge.start(id);
        return 1;
    } catch {
        return 0;
    }
});
EM_JS(int, rtc_description, (unsigned id, const char *data, unsigned size), {
    try {
        Module.p2pdcWebRtcBridge.description(id, UTF8ToString(data, size));
        return 1;
    } catch {
        return 0;
    }
});
EM_JS(int, rtc_candidate,
      (unsigned id, unsigned operation, const char *data, unsigned size, const char *mid,
       unsigned mid_size),
      {
          try {
              Module.p2pdcWebRtcBridge.candidate(id, operation, UTF8ToString(data, size),
                                                 UTF8ToString(mid, mid_size));
              return 1;
          } catch {
              return 0;
          }
      });
EM_JS(int, rtc_send, (unsigned id, const std::byte *data, unsigned size), {
    try {
        return Module.p2pdcWebRtcBridge.send(id, HEAPU8.slice(data, data + size));
    } catch {
        return 4;
    }
});
EM_JS(double, rtc_measure, (unsigned id, unsigned maximum), {
    try {
        const value =
            maximum ? Module.p2pdcWebRtcBridge.maximum(id) : Module.p2pdcWebRtcBridge.buffered(id);
        if (value == null || Number.isNaN(value) || value < 0)
            return -1;
        if (!Number.isFinite(value))
            return maximum ? 0 : -1;
        return Math.min(value, 4294967295);
    } catch {
        return -1;
    }
});
EM_JS(int, rtc_poll, (unsigned id), {
    try {
        Module.p2pdcWebRtcBridge.poll(id);
        return 1;
    } catch {
        return 0;
    }
});
EM_JS(void, rtc_close, (unsigned id), {
    try {
        Module.p2pdcWebRtcBridge.close(id);
    } catch {
        console.error("p2pdc: WebRTC transport cleanup failed");
    }
});

namespace p2pdc::detail {
bool browser_webrtc_available() { return rtc_available() != 0; }

struct BrowserTransportImpl {
    unsigned id = 0;
    std::size_t max_message = 0;
    BrowserTransportCallbacks callbacks;
    bool closed = false;
    bool event(unsigned kind, unsigned value, ByteView first, ByteView second);
    bool reject_event();
    bool control(unsigned kind, unsigned value);
    void close();
};
namespace {
std::map<unsigned, std::weak_ptr<BrowserTransportImpl>> transports;
std::string string(ByteView bytes) {
    return bytes.empty() ? std::string{}
                         : std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
bool valid_text(std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum && text.find('\0') == std::string_view::npos;
}
void validate(BrowserRole role, BrowserTransportLimits limits, const BrowserTransportCallbacks &c) {
    if ((role != BrowserRole::Offerer && role != BrowserRole::Answerer) || !limits.message_bytes ||
        limits.message_bytes > 1048576 || !limits.buffer_bytes || limits.buffer_bytes > 16777216 ||
        !limits.inbound_bytes || limits.inbound_bytes > 16777216 || !limits.inbound_messages ||
        limits.inbound_messages > 1024 || limits.message_bytes > limits.buffer_bytes ||
        limits.message_bytes > limits.inbound_bytes || !c.on_description || !c.on_candidate ||
        !c.on_state || !c.on_binary || !c.on_route || !c.on_error || !c.on_writable ||
        !c.on_description_applied || !c.on_candidate_applied)
        throw std::invalid_argument("invalid browser transport settings");
}
CandidateKind candidate_kind(unsigned value) {
    switch (value) {
    case 1:
        return CandidateKind::Host;
    case 2:
        return CandidateKind::ServerReflexive;
    case 3:
        return CandidateKind::PeerReflexive;
    case 4:
        return CandidateKind::Relayed;
    default:
        return CandidateKind::Unknown;
    }
}
RouteInfo route(unsigned value) {
    RouteInfo info;
    const auto kind = value & 3U;
    if (kind == 1)
        info.current = RouteKind::Direct;
    if (kind == 2)
        info.current = RouteKind::TurnRelayed;
    if (value & 4U)
        info.turn_history = TurnHistory::Used;
    if (value & 8U) {
        info.selected_pair =
            CandidatePair{candidate_kind((value >> 4U) & 15U), candidate_kind((value >> 8U) & 15U)};
        info.observed_at = std::chrono::steady_clock::now();
    }
    return info;
}
void configure(unsigned id, const std::vector<BrowserIceServer> &servers) {
    for (const auto &s : servers) {
        if (!rtc_server(id, s.url.data(), s.url.size(), s.username.data(), s.username.size(),
                        s.password.data(), s.password.size()))
            throw std::runtime_error("browser ICE configuration failed");
    }
    if (!rtc_open(id))
        throw std::runtime_error("browser WebRTC initialization failed");
}
} // namespace

bool BrowserTransportImpl::event(unsigned kind, unsigned value, ByteView first, ByteView second) {
    if (closed)
        return false;
    if ((kind == 0 && (first.empty() || first.size() > 32768)) ||
        (kind == 1 &&
         (first.empty() || first.size() > 4096 || second.empty() || second.size() > 256)) ||
        (kind == 3 && first.size() > max_message)) {
        callbacks.on_error(BrowserTransportError::Transport);
        close();
        return false;
    }
    switch (kind) {
    case 0:
        callbacks.on_description(string(first));
        break;
    case 1:
        callbacks.on_candidate(string(first), string(second));
        break;
    case 2:
        if (value > 3)
            return reject_event();
        callbacks.on_state(static_cast<BrowserTransportState>(value));
        break;
    case 3:
        callbacks.on_binary(Bytes(first.begin(), first.end()));
        break;
    case 4:
        callbacks.on_route(route(value));
        break;
    default:
        return control(kind, value);
    }
    return !closed;
}
bool BrowserTransportImpl::control(unsigned kind, unsigned value) {
    switch (kind) {
    case 5:
        callbacks.on_error(value == 1 ? BrowserTransportError::Resource
                                      : BrowserTransportError::Transport);
        break;
    case 6:
        callbacks.on_writable();
        break;
    case 7:
        callbacks.on_description_applied(value == 1);
        break;
    case 8:
        callbacks.on_candidate_applied(value, true);
        break;
    case 9:
        callbacks.on_candidate_applied(value, false);
        break;
    default:
        return reject_event();
    }
    return !closed;
}
// An event the bridge should never produce means the JS and C++ halves
// disagree; report it as a transport failure rather than closing silently.
bool BrowserTransportImpl::reject_event() {
    callbacks.on_error(BrowserTransportError::Transport);
    close();
    return false;
}
void BrowserTransportImpl::close() {
    if (closed)
        return;
    closed = true;
    transports.erase(id);
    rtc_close(id);
}
BrowserTransport::BrowserTransport(BrowserRole role, IceConfig ice, BrowserTransportLimits limits,
                                   BrowserTransportCallbacks callbacks) {
    validate(role, limits, callbacks);
    const auto servers = browser_ice_servers(ice);
    const auto id = rtc_begin(
        role == BrowserRole::Answerer, ice.relay_policy == RelayPolicy::RequireLocalRelay,
        limits.message_bytes, limits.buffer_bytes, limits.inbound_bytes, limits.inbound_messages);
    if (!id)
        throw std::runtime_error("browser WebRTC transport limit reached");
    try {
        auto state = std::make_shared<BrowserTransportImpl>();
        state->id = id;
        state->max_message = limits.message_bytes;
        state->callbacks = std::move(callbacks);
        transports.emplace(id, state);
        configure(id, servers);
        state_ = std::move(state);
    } catch (...) {
        transports.erase(id);
        rtc_close(id);
        throw;
    }
}
BrowserTransport::~BrowserTransport() { close(); }
bool BrowserTransport::start_offer() { return !state_->closed && rtc_start(state_->id); }
bool BrowserTransport::set_remote_description(std::string_view sdp) {
    return !state_->closed && valid_text(sdp, 32768) &&
           rtc_description(state_->id, sdp.data(), sdp.size());
}
bool BrowserTransport::add_remote_candidate(std::uint32_t operation, std::string_view candidate,
                                            std::string_view mid) {
    return !state_->closed && operation && valid_text(candidate, 4096) && valid_text(mid, 256) &&
           rtc_candidate(state_->id, operation, candidate.data(), candidate.size(), mid.data(),
                         mid.size());
}
BrowserSend BrowserTransport::send(ByteView bytes) {
    if (state_->closed)
        return BrowserSend::Closed;
    if (bytes.size() > state_->max_message)
        return BrowserSend::TooLarge;
    const int result = rtc_send(state_->id, bytes.data(), bytes.size());
    return result >= 0 && result <= 5 ? static_cast<BrowserSend>(result) : BrowserSend::Failed;
}
std::optional<std::size_t> BrowserTransport::buffered_amount() const {
    if (state_->closed)
        return std::nullopt;
    const auto value = rtc_measure(state_->id, 0);
    return value < 0 ? std::nullopt : std::optional{static_cast<std::size_t>(value)};
}
std::optional<std::size_t> BrowserTransport::max_message_size() const {
    if (state_->closed)
        return std::nullopt;
    const auto value = rtc_measure(state_->id, 1);
    return value < 0 ? std::nullopt : std::optional{static_cast<std::size_t>(value)};
}
void BrowserTransport::poll_route() {
    if (!state_->closed && !rtc_poll(state_->id))
        throw std::runtime_error("WebRTC route polling failed");
}
void BrowserTransport::close() {
    if (state_)
        state_->close();
}
} // namespace p2pdc::detail

extern "C" {
EMSCRIPTEN_KEEPALIVE void *p2pdc_webrtc_alloc(unsigned size) {
    return std::malloc(std::max(1U, size));
}
EMSCRIPTEN_KEEPALIVE void p2pdc_webrtc_free(void *value) { std::free(value); }
EMSCRIPTEN_KEEPALIVE int p2pdc_webrtc_event(unsigned id, unsigned kind, unsigned value,
                                            const std::byte *a, unsigned a_size, const std::byte *b,
                                            unsigned b_size) {
    auto found = p2pdc::detail::transports.find(id);
    if (found == p2pdc::detail::transports.end())
        return 0;
    auto state = found->second.lock();
    if (!state || (!a && a_size) || (!b && b_size))
        return 0;
    try {
        if (state->event(kind, value, {a, a_size}, {b, b_size}))
            return 1;
        state->close();
        return 0;
    } catch (...) {
        std::cerr << "p2pdc: WebRTC event delivery failed\n";
        state->close();
        return 0;
    }
}
}
