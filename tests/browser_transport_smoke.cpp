#include "../src/browser/browser_transport.hpp"
#include "p2pdc/runtime.h"

#include <array>
#include <chrono>
#include <emscripten.h>
#include <iostream>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

EM_JS(void, fail_latest_transport, (int resource), {
    const entries = Array.from(Module.p2pdcWebRtcBridge.entries.values());
    entries[entries.length - 1].transport.fail("test failure", resource ? "resource" : "transport");
});

namespace {
struct Probe : std::enable_shared_from_this<Probe> {
    Probe(Runtime &runtime, bool relay, std::function<void(bool)> done)
        : runtime(runtime), done(std::move(done)), relay(relay) {}
    Runtime &runtime;
    std::function<void(bool)> done;
    std::shared_ptr<Probe> keepalive;
    std::array<std::unique_ptr<BrowserTransport>, 2> peer;
    std::array<std::unique_ptr<BrowserTransport>, 2> fault;
    std::array<bool, 2> ready{}, received{}, direct{};
    std::array<bool, 2> error_seen{}, failure_seen{};
    CancelTimer deadline;
    bool sent = false, finished = false, relay = false;
    unsigned candidate_id = 0;
    unsigned descriptions = 0, candidates = 0, errors = 0;
    BrowserTransportCallbacks handlers(unsigned index);
    bool rejects_invalid_settings();
    void check_errors();
    void start();
    void description(unsigned index, std::string value);
    void candidate(unsigned index, std::string value, std::string mid);
    void state(unsigned index, BrowserTransportState value);
    void message(unsigned index, Bytes value);
    void route(unsigned index, RouteInfo value);
    void check();
    void finish(bool success);
    void cancel();
};
BrowserTransportCallbacks Probe::handlers(unsigned index) {
    auto weak = weak_from_this();
    return {[weak, index](std::string value) {
                if (auto self = weak.lock())
                    self->description(index, std::move(value));
            },
            [weak, index](std::string value, std::string mid) {
                if (auto self = weak.lock())
                    self->candidate(index, std::move(value), std::move(mid));
            },
            [weak, index](BrowserTransportState value) {
                if (auto self = weak.lock())
                    self->state(index, value);
            },
            [weak, index](Bytes value) {
                if (auto self = weak.lock())
                    self->message(index, std::move(value));
            },
            [weak, index](RouteInfo value) {
                if (auto self = weak.lock())
                    self->route(index, std::move(value));
            },
            [weak](BrowserTransportError) {
                if (auto self = weak.lock()) {
                    ++self->errors;
                    self->finish(false);
                }
            },
            [] {},
            [weak](bool ok) {
                if (auto self = weak.lock(); self && !ok)
                    self->finish(false);
            },
            [weak](std::uint32_t, bool ok) {
                if (auto self = weak.lock(); self && !ok)
                    self->finish(false);
            }};
}
bool Probe::rejects_invalid_settings() {
    auto invalid = handlers(0);
    invalid.on_error = {};
    bool invalid_callbacks = false, invalid_relay = false;
    try {
        BrowserTransport rejected(BrowserRole::Offerer, {}, {}, invalid);
    } catch (const std::invalid_argument &) {
        invalid_callbacks = true;
    }
    try {
        BrowserTransport rejected(BrowserRole::Offerer, {{}, RelayPolicy::RequireLocalRelay}, {},
                                  handlers(0));
    } catch (const std::invalid_argument &) {
        invalid_relay = true;
    }
    return invalid_callbacks && invalid_relay;
}
void Probe::check_errors() {
    auto weak = weak_from_this();
    for (unsigned index = 0; index < 2; ++index) {
        auto callbacks = handlers(index);
        callbacks.on_error = [weak, index](BrowserTransportError error) {
            if (auto self = weak.lock()) {
                self->error_seen[index] = error == (index ? BrowserTransportError::Resource
                                                          : BrowserTransportError::Transport);
            }
        };
        callbacks.on_state = [weak, index](BrowserTransportState state) {
            if (auto self = weak.lock(); self && state == BrowserTransportState::Failed) {
                self->failure_seen[index] = self->error_seen[index];
                self->check();
            }
        };
        fault[index] = std::make_unique<BrowserTransport>(BrowserRole::Answerer, IceConfig{},
                                                          BrowserTransportLimits{}, callbacks);
        fail_latest_transport(index);
    }
}
void Probe::start() {
    keepalive = shared_from_this();
    if (!rejects_invalid_settings()) {
        finish(false);
        return;
    }
    check_errors();
    IceConfig ice;
    if (relay) {
        ice.servers = {
            TurnServer{"10.44.0.1", 34789, "p2pdc", "temporary-test-password", TurnTransport::Tcp}};
        ice.relay_policy = RelayPolicy::RequireLocalRelay;
    }
    peer[0] = std::make_unique<BrowserTransport>(BrowserRole::Offerer, ice,
                                                 BrowserTransportLimits{}, handlers(0));
    peer[1] = std::make_unique<BrowserTransport>(BrowserRole::Answerer, ice,
                                                 BrowserTransportLimits{}, handlers(1));
    if (peer[0]->send(Bytes{std::byte{1}}) != BrowserSend::NotReady ||
        peer[0]->set_remote_description("") || peer[0]->add_remote_candidate(0, "x", "0") ||
        peer[0]->send(Bytes(262146)) != BrowserSend::TooLarge || !peer[0]->start_offer()) {
        finish(false);
        return;
    }
    auto weak = weak_from_this();
    deadline = runtime.scheduler()(8s, [weak] {
        if (auto self = weak.lock())
            self->finish(false);
    });
}
void Probe::description(unsigned index, std::string value) {
    ++descriptions;
    if (!peer[1 - index]->set_remote_description(value))
        finish(false);
}
void Probe::candidate(unsigned index, std::string value, std::string mid) {
    ++candidates;
    if (!peer[1 - index]->add_remote_candidate(++candidate_id, value, mid))
        finish(false);
}
void Probe::state(unsigned index, BrowserTransportState value) {
    if (value == BrowserTransportState::Failed) {
        finish(false);
        return;
    }
    if (value != BrowserTransportState::Ready)
        return;
    ready[index] = true;
    peer[index]->poll_route();
    if (ready[0] && ready[1] && !sent) {
        sent = true;
        if (peer[0]->send(Bytes{std::byte{0}, std::byte{255}}) != BrowserSend::Accepted ||
            peer[1]->send(Bytes{std::byte{42}}) != BrowserSend::Accepted)
            finish(false);
    }
}
void Probe::message(unsigned index, Bytes value) {
    const Bytes expected = index == 0 ? Bytes{std::byte{42}} : Bytes{std::byte{0}, std::byte{255}};
    if (value != expected) {
        finish(false);
        return;
    }
    received[index] = true;
    check();
}
void Probe::route(unsigned index, RouteInfo value) {
    direct[index] =
        value.selected_pair &&
        (relay ? value.current == RouteKind::TurnRelayed &&
                     value.selected_pair->local == CandidateKind::Relayed &&
                     value.turn_history == TurnHistory::Used
               : value.current == RouteKind::Direct && value.turn_history == TurnHistory::Unknown);
    check();
}
void Probe::check() {
    if (ready[0] && ready[1] && received[0] && received[1] && direct[0] && direct[1] &&
        failure_seen[0] && failure_seen[1])
        finish(peer[0]->buffered_amount().has_value() && peer[1]->max_message_size().has_value());
}
void Probe::finish(bool success) {
    if (finished)
        return;
    if (!success)
        std::cerr << "rtc test desc=" << descriptions << " candidates=" << candidates
                  << " ready=" << ready[0] << ready[1] << " received=" << received[0] << received[1]
                  << " route=" << direct[0] << direct[1] << " errors=" << errors << '\n';
    finished = true;
    if (deadline)
        deadline();
    for (auto &value : peer)
        if (value)
            value->close();
    for (auto &value : fault)
        if (value)
            value->close();
    done(success);
    keepalive.reset();
}
void Probe::cancel() {
    if (finished)
        return;
    finished = true;
    if (deadline)
        deadline();
    for (auto &value : peer)
        if (value)
            value->close();
    for (auto &value : fault)
        if (value)
            value->close();
    keepalive.reset();
}
} // namespace

std::function<void()> run_browser_transport_smoke(Runtime &runtime, bool relay,
                                                  std::function<void(bool)> done) {
    auto probe = std::make_shared<Probe>(runtime, relay, std::move(done));
    std::weak_ptr<Probe> weak = probe;
    probe->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
