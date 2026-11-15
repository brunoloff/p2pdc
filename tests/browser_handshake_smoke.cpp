#include "../src/security/handshake_gate.hpp"

#include <emscripten.h>

#include <deque>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

bool secure_channel_smoke();
bool portable_admission_smoke();
bool browser_public_config_smoke();
bool record_store_smoke();

EM_JS(void, report, (int success), {
    document.body.dataset.result = success ? "pass" : "fail";
    document.body.textContent = success ? "pass" : "fail: handshake check";
});

namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("browser handshake check failed");
}
struct Transport final : HandshakeTransport {
    bool closed = false;
    std::deque<Bytes> sent;
    ChannelSend send(ByteView bytes) override {
        if (closed)
            return ChannelSend::Closed;
        sent.emplace_back(bytes.begin(), bytes.end());
        return ChannelSend::Accepted;
    }
    void close() override { closed = true; }
};
struct Pair {
    Identity left_id = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity right_id = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    Transport left, right;
    std::unique_ptr<HandshakeGate> a, b;
    unsigned ready = 0, errors = 0;
    bool valid_limits = true;
    std::vector<Bytes> received;
    HandshakeContext context() const {
        return {left_id.public_key(), right_id.public_key(), {std::byte{1}}, {std::byte{2}},
                {std::byte{3}},       {std::byte{4}},        {std::byte{5}}, {std::byte{6}}};
    }
    HandshakeGateCallbacks callbacks() {
        return {[this](std::uint32_t limit) {
                    valid_limits = valid_limits && limit == 80;
                    ++ready;
                },
                [this](Bytes message) { received.push_back(std::move(message)); }, [] {},
                [this](GateError) { ++errors; }};
    }
    void create(bool mismatch) {
        auto right_context = context();
        if (mismatch)
            right_context.answer_hash[0] ^= std::byte{1};
        a = std::make_unique<HandshakeGate>(left_id, context(), HandshakeRole::Offerer, 100, 200,
                                            left, callbacks());
        b = std::make_unique<HandshakeGate>(right_id, right_context, HandshakeRole::Answerer, 80,
                                            200, right, callbacks());
    }
    void deliver() {
        for (int count = 0; count < 16 && (!left.sent.empty() || !right.sent.empty()); ++count) {
            if (!left.sent.empty()) {
                auto message = std::move(left.sent.front());
                left.sent.pop_front();
                b->on_message(message);
            }
            if (!right.sent.empty()) {
                auto message = std::move(right.sent.front());
                right.sent.pop_front();
                a->on_message(message);
            }
        }
    }
};
void check_matching_context() {
    Pair pair;
    pair.create(false);
    require(pair.a->send({}) == ChannelSend::NotReady);
    pair.a->set_transport_ready(true);
    pair.b->set_transport_ready(true);
    pair.deliver();
    require(pair.a->ready() && pair.b->ready() && pair.ready == 2 && pair.errors == 0 &&
            pair.valid_limits);
    require(pair.a->send(Bytes(81)) == ChannelSend::TooLarge);
    const Bytes value{std::byte{0}, std::byte{255}};
    require(pair.a->send(value) == ChannelSend::Accepted);
    pair.deliver();
    require(pair.received.size() == 1 && pair.received.front() == value);
}
void check_mismatched_context() {
    Pair pair;
    pair.create(true);
    pair.a->set_transport_ready(true);
    pair.b->set_transport_ready(true);
    pair.deliver();
    require(pair.errors > 0 && pair.ready == 0 && pair.received.empty());
}
} // namespace

int main() {
    try {
        check_matching_context();
        check_mismatched_context();
        require(secure_channel_smoke());
        require(portable_admission_smoke());
        require(browser_public_config_smoke());
        require(record_store_smoke());
        report(1);
        return 0;
    } catch (const std::exception &) {
        report(0);
        return 1;
    }
}
