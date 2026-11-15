#include "../src/security/secure_attempt.hpp"

#include <deque>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Loop {
    std::deque<std::function<void()>> tasks;
    Executor executor() {
        return [this](auto task) { tasks.push_back(std::move(task)); };
    }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
};
struct Sink final : ReliableSink {
    std::deque<Bytes> frames;
    std::size_t buffered_amount() const override { return 0; }
    SinkResult send(ByteView frame) override {
        frames.emplace_back(frame.begin(), frame.end());
        return SinkResult::Accepted;
    }
};
struct Fixture {
    Identity a = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity b = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    RecipientKeyPair box_a =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    RecipientKeyPair box_b =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    Bytes capability = Bytes(32, std::byte{5});
    SignalingRecipient peer_a{a.public_key(), {std::byte{1}}};
    SignalingRecipient peer_b{b.public_key(), {std::byte{2}}};
    SignalingPolicy policy{100, 60, 5, 4096};
    std::vector<Bytes> from_a, from_b, received;
    unsigned ready = 0, failures = 0;
    Loop loop;
    Sink sink_a, sink_b;
    SignalingFlowCallbacks signals(std::vector<Bytes> &out) {
        return {[&out](Bytes wire) {
                    out.push_back(std::move(wire));
                    return true;
                },
                [](ByteView) { return true; }, [](CandidateView) { return true; },
                [](SignalingKind) { return true; }};
    }
    SecureAttemptCallbacks attempts(std::vector<Bytes> &out) {
        return {signals(out),
                [this](std::uint32_t limit) {
                    if (limit == 100)
                        ++ready;
                },
                [this](Bytes value) { received.push_back(std::move(value)); }, [] {},
                [this](AttemptFailure) { ++failures; }};
    }
};
bool accepted(FlowResult value) { return value == FlowResult::Accepted; }
void transfer(Loop &loop, Sink &a, Sink &b, SecureAttempt &left, SecureAttempt &right) {
    for (int count = 0; count < 100; ++count) {
        loop.drain();
        if (a.frames.empty() && b.frames.empty())
            return;
        while (!a.frames.empty()) {
            auto frame = std::move(a.frames.front());
            a.frames.pop_front();
            right.on_frame(frame);
        }
        while (!b.frames.empty()) {
            auto frame = std::move(b.frames.front());
            b.frames.pop_front();
            left.on_frame(frame);
        }
    }
}
} // namespace

bool secure_channel_smoke() {
    Fixture f;
    const AsyncSignalingBinding ab{f.peer_a,
                                   f.peer_b,
                                   f.box_a.public_key(),
                                   f.box_b.public_key(),
                                   {std::byte{3}},
                                   HandshakeRole::Offerer};
    const AsyncSignalingBinding ba{f.peer_b,
                                   f.peer_a,
                                   f.box_b.public_key(),
                                   f.box_a.public_key(),
                                   {std::byte{3}},
                                   HandshakeRole::Answerer};
    const ReliableChannelLimits limits{{201, 64}, 4096, 16, 256, 4};
    SecureAttempt a(ab, f.capability, f.a, f.box_a, {128, 32, 256}, MemoryBudget(8192), limits, 100,
                    f.sink_a, f.loop.executor(), f.attempts(f.from_a));
    SecureAttempt b(ba, f.capability, f.b, f.box_b, {128, 32, 256}, MemoryBudget(8192), limits, 100,
                    f.sink_b, f.loop.executor(), f.attempts(f.from_b));
    if (!accepted(a.local_description(Bytes{std::byte{1}}, 100, 150, f.policy)) ||
        !accepted(b.receive_signaling(f.from_a.at(0), f.policy)) ||
        !accepted(b.description_applied(true)) ||
        !accepted(b.local_description(Bytes{std::byte{2}}, 100, 150, f.policy)) ||
        !accepted(a.receive_signaling(f.from_b.at(0), f.policy)) ||
        !accepted(a.description_applied(true)) || a.send({}) != ChannelSend::NotReady)
        return false;
    a.set_transport_ready(true);
    b.set_transport_ready(true);
    transfer(f.loop, f.sink_a, f.sink_b, a, b);
    if (!a.ready() || !b.ready() || f.ready != 2 || f.failures)
        return false;
    const Bytes message(90, std::byte{255});
    if (a.send(message) != ChannelSend::Accepted)
        return false;
    transfer(f.loop, f.sink_a, f.sink_b, a, b);
    return f.received.size() == 1 && f.received.front() == message && !f.failures;
}
