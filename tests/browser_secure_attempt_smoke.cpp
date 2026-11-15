#include "../src/browser/browser_secure_attempt.hpp"
#include "p2pdc/runtime.h"

#include <array>
#include <chrono>
#include <emscripten.h>
#include <iostream>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

EM_JS(void, fail_latest_secure_transport, (int resource), {
    const entries = Array.from(Module.p2pdcWebRtcBridge.entries.values());
    entries[entries.length - 1].transport.fail("test failure", resource ? "resource" : "transport");
});

namespace {
std::uint64_t now_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("invalid test clock");
    return static_cast<std::uint64_t>(value);
}
struct Probe : std::enable_shared_from_this<Probe> {
    Probe(Runtime &runtime, bool relay, std::function<void(bool)> done)
        : runtime(runtime), relay(relay), done(std::move(done)) {}
    Runtime &runtime;
    bool relay;
    std::function<void(bool)> done;
    std::shared_ptr<Probe> keepalive;
    std::array<std::shared_ptr<Identity>, 2> identity;
    std::array<std::shared_ptr<RecipientKeyPair>, 2> boxes;
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{5});
    std::array<std::unique_ptr<BrowserSecureAttempt>, 2> peers;
    std::array<std::unique_ptr<BrowserSecureAttempt>, 2> fault;
    std::array<MemoryBudget, 2> budgets{MemoryBudget(4194304), MemoryBudget(4194304)};
    std::array<bool, 2> ready{}, received{};
    std::array<bool, 2> classified{};
    CancelTimer deadline;
    bool sent = false, tampered = false, ignored = false, finished = false;
    unsigned signals = 0, failures = 0;
    BrowserSecureConfig config(unsigned index) const;
    BrowserSecureCallbacks handlers(unsigned index);
    bool submit(unsigned index, Bytes value);
    void deliver(unsigned index, Bytes value, bool corrupt);
    bool errors();
    bool check_failure_categories();
    void start();
    void on_ready(unsigned index, std::uint32_t limit);
    void transmit();
    void on_message(unsigned index, Bytes message);
    void check();
    void finish(bool result);
    void cancel();
};
BrowserSecureConfig Probe::config(unsigned index) const {
    const auto other = 1U - index;
    AsyncSignalingBinding binding{
        {identity[index]->public_key(), {static_cast<std::byte>(index + 1)}},
        {identity[other]->public_key(), {static_cast<std::byte>(other + 1)}},
        boxes[index]->public_key(),
        boxes[other]->public_key(),
        AttemptId{std::byte{7}},
        index ? HandshakeRole::Answerer : HandshakeRole::Offerer};
    IceConfig ice;
    if (relay) {
        ice.servers = {
            TurnServer{"10.44.0.1", 34789, "p2pdc", "temporary-test-password", TurnTransport::Tcp}};
        ice.relay_policy = RelayPolicy::RequireLocalRelay;
    }
    return {binding,        identity[index],         boxes[index], capability,
            std::move(ice), index ? 32768U : 65536U, now_seconds};
}
BrowserSecureCallbacks Probe::handlers(unsigned index) {
    auto weak = weak_from_this();
    return {[weak, index](Bytes value) {
                if (auto self = weak.lock())
                    return self->submit(index, std::move(value));
                return false;
            },
            [weak, index](std::uint32_t value) {
                if (auto self = weak.lock())
                    self->on_ready(index, value);
            },
            [weak, index](Bytes value) {
                if (auto self = weak.lock())
                    self->on_message(index, std::move(value));
            },
            [] {},
            [](RouteInfo) {},
            [](BrowserTransportState) {},
            [weak](AttemptFailure) {
                if (auto self = weak.lock()) {
                    ++self->failures;
                    self->finish(false);
                }
            }};
}
bool Probe::submit(unsigned index, Bytes value) {
    ++signals;
    auto weak = weak_from_this();
    if (!tampered && index == 0) {
        tampered = true;
        auto bad = value;
        bad.back() ^= std::byte{1};
        runtime.executor()([weak, index, bad = std::move(bad)]() mutable {
            if (auto self = weak.lock())
                self->deliver(index, std::move(bad), true);
        });
    }
    runtime.executor()([weak, index, value = std::move(value)]() mutable {
        if (auto self = weak.lock())
            self->deliver(index, std::move(value), false);
    });
    return true;
}
void Probe::deliver(unsigned index, Bytes value, bool corrupt) {
    if (finished)
        return;
    const auto result = peers[1 - index]->receive(value);
    if (corrupt) {
        ignored = result == FlowResult::Ignored;
        if (!ignored)
            finish(false);
    } else if (result != FlowResult::Accepted && result != FlowResult::Ignored)
        finish(false);
}
bool Probe::errors() {
    auto mismatch = config(1);
    mismatch.binding.local.signing_key = identity[0]->public_key();
    bool rejected = false;
    try {
        BrowserSecureAttempt invalid(std::move(mismatch), budgets[1], runtime.executor(),
                                     handlers(1));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected)
        return false;
    auto value = config(1);
    unsigned clock_calls = 0;
    value.now = [&clock_calls] { return clock_calls++ ? 99U : 100U; };
    bool failed = false;
    auto callbacks = handlers(1);
    callbacks.on_failure = [&failed](AttemptFailure reason) {
        failed = reason == AttemptFailure::Signaling;
    };
    BrowserSecureAttempt regression(std::move(value), budgets[1], runtime.executor(),
                                    std::move(callbacks));
    if (!regression.start() || regression.receive(Bytes{std::byte{0}}) != FlowResult::Ignored ||
        regression.receive(Bytes{std::byte{0}}) != FlowResult::Failed || !failed ||
        regression.send({}) != ChannelSend::Failed)
        return false;
    regression.close();
    if (regression.send({}) != ChannelSend::Closed)
        return false;
    failed = false;
    auto timeout_handlers = handlers(1);
    timeout_handlers.on_failure = [&failed](AttemptFailure reason) {
        failed = reason == AttemptFailure::Timeout;
    };
    BrowserSecureAttempt timeout(config(1), budgets[1], runtime.executor(),
                                 std::move(timeout_handlers));
    if (!timeout.start())
        return false;
    timeout.timeout();
    return failed && !timeout.ready();
}
bool Probe::check_failure_categories() {
    auto weak = weak_from_this();
    for (unsigned index = 0; index < 2; ++index) {
        auto callbacks = handlers(1);
        callbacks.on_failure = [weak, index](AttemptFailure reason) {
            if (auto self = weak.lock()) {
                self->classified[index] =
                    reason == (index ? AttemptFailure::Resource : AttemptFailure::Transport);
                self->check();
            }
        };
        fault[index] = std::make_unique<BrowserSecureAttempt>(config(1), MemoryBudget(4194304),
                                                              runtime.executor(), callbacks);
        if (!fault[index]->start())
            return false;
        fail_latest_secure_transport(index);
    }
    return true;
}
void Probe::start() {
    keepalive = shared_from_this();
    for (unsigned i = 0; i < 2; ++i) {
        identity[i] = std::make_shared<Identity>(
            std::get<Identity>(Identity::import_seed(Bytes(32, static_cast<std::byte>(i + 1)))));
        boxes[i] = std::make_shared<RecipientKeyPair>(std::get<RecipientKeyPair>(
            RecipientKeyPair::import_secret_key(Bytes(32, static_cast<std::byte>(i + 3)))));
    }
    if (!errors() || !check_failure_categories()) {
        finish(false);
        return;
    }
    for (unsigned i = 0; i < 2; ++i)
        peers[i] = std::make_unique<BrowserSecureAttempt>(config(i), budgets[i], runtime.executor(),
                                                          handlers(i));
    if (!peers[1]->start() || !peers[0]->start() || peers[0]->send({}) != ChannelSend::NotReady) {
        finish(false);
        return;
    }
    auto weak = weak_from_this();
    deadline = runtime.scheduler()(8s, [weak] {
        if (auto self = weak.lock())
            self->finish(false);
    });
}
void Probe::on_ready(unsigned index, std::uint32_t limit) {
    if (limit != 32768 || peers[index]->max_message_size() != limit) {
        finish(false);
        return;
    }
    ready[index] = true;
    if (ready[0] && ready[1] && !sent) {
        sent = true;
        auto weak = weak_from_this();
        runtime.executor()([weak] {
            if (auto self = weak.lock())
                self->transmit();
        });
    }
}
void Probe::transmit() {
    auto held = budgets[0].try_reserve(budgets[0].limit() - budgets[0].used());
    if (!held || peers[0]->send(Bytes{std::byte{1}}) != ChannelSend::Backpressure) {
        finish(false);
        return;
    }
    held.reset();
    if (peers[0]->send(Bytes(16000, std::byte{255})) != ChannelSend::Accepted ||
        peers[1]->send(Bytes{std::byte{0}, std::byte{42}}) != ChannelSend::Accepted ||
        peers[0]->send(Bytes(32769)) != ChannelSend::TooLarge)
        finish(false);
}
void Probe::on_message(unsigned index, Bytes message) {
    const Bytes expected =
        index == 0 ? Bytes{std::byte{0}, std::byte{42}} : Bytes(16000, std::byte{255});
    if (message != expected) {
        finish(false);
        return;
    }
    received[index] = true;
    check();
}
void Probe::check() {
    if (ready[0] && ready[1] && received[0] && received[1] && ignored && classified[0] &&
        classified[1]) {
        auto weak = weak_from_this();
        runtime.executor()([weak] {
            if (auto self = weak.lock())
                self->finish(self->peers[0]->ready() && self->peers[1]->ready());
        });
    }
}
void Probe::finish(bool result) {
    if (finished)
        return;
    if (!result)
        std::cerr << "secure test signals=" << signals << " ready=" << ready[0] << ready[1]
                  << " received=" << received[0] << received[1] << " failures=" << failures << '\n';
    finished = true;
    if (deadline)
        deadline();
    for (auto &peer : fault)
        if (peer)
            peer->close();
    for (auto &peer : peers) {
        if (!peer)
            continue;
        if (result && relay && peer->route().turn_history != TurnHistory::Used)
            result = false;
        peer->close();
        if (result &&
            (peer->ready() || peer->max_message_size() || peer->send({}) != ChannelSend::Closed ||
             peer->route().current != RouteKind::Unknown ||
             (relay && peer->route().turn_history != TurnHistory::Used)))
            result = false;
    }
    done(result);
    keepalive.reset();
}
void Probe::cancel() {
    if (finished)
        return;
    finished = true;
    if (deadline)
        deadline();
    for (auto &peer : fault)
        if (peer)
            peer->close();
    for (auto &peer : peers)
        if (peer)
            peer->close();
    keepalive.reset();
}
} // namespace

std::function<void()> run_browser_secure_attempt_smoke(Runtime &runtime, bool relay,
                                                       std::function<void(bool)> done) {
    auto probe = std::make_shared<Probe>(runtime, relay, std::move(done));
    std::weak_ptr<Probe> weak = probe;
    probe->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
