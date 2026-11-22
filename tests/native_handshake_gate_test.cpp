#include "../src/security/handshake_gate.hpp"
#include "../src/security/signaling_candidate.hpp"
#include "../src/security/signaling_session.hpp"
#include "../src/transport/native_reliable_link.hpp"
#include "../src/transport/native_signaling.hpp"
#include "borrowed.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    void drain() {
        for (int count = 0; count < 10000; ++count) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        drain();
        return done();
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

struct Adapter : HandshakeTransport {
    NativeReliableLink *link = nullptr;
    ChannelSend send(ByteView bytes) override { return link->send(bytes); }
    void limit_receive(std::size_t bytes) override { link->set_receive_limit(bytes); }
    void close() override { link->close(); }
};

struct Pair {
    Identity a = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity b = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    RecipientKeyPair box_a =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    RecipientKeyPair box_b =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{5})));
    Bytes capability = Bytes(32, std::byte{3});
    SignalingPolicy policy{100, 60, 5, 65536};
    std::unique_ptr<SignalingSession> session_a;
    std::unique_ptr<SignalingSession> session_b;
    std::unique_ptr<NativeSignaling> signaling_a;
    std::unique_ptr<NativeSignaling> signaling_b;
    std::unique_ptr<CandidateBuffer> candidates_a;
    std::unique_ptr<CandidateBuffer> candidates_b;
    Loop loop;
    std::unique_ptr<NativeReliableLink> left;
    std::unique_ptr<NativeReliableLink> right;
    Adapter left_adapter;
    Adapter right_adapter;
    std::unique_ptr<HandshakeGate> left_gate;
    std::unique_ptr<HandshakeGate> right_gate;
    std::vector<Bytes> received;
    std::vector<GateError> errors;
    int ready_count = 0;
    int candidate_count = 0;
    bool mismatch = false;
    bool use_signaling = false;
    bool bridge_offer_delayed = false;
    bool defer_offer = false;
    bool reordered_candidate = false;
    std::optional<Bytes> pending_offer;

    HandshakeContext context(bool first) {
        auto &session = first ? session_a : session_b;
        auto result = use_signaling
                          ? (first ? signaling_a : signaling_b)->handshake_context().value()
                          : std::get<HandshakeContext>(session->handshake_context());
        if (!first && mismatch) {
            result.answer_hash[0] ^= std::byte{1};
        }
        return result;
    }
    HandshakeGateCallbacks gate_callbacks() {
        return {[this](std::uint32_t maximum) {
                    EXPECT_EQ(maximum, 1024U);
                    ++ready_count;
                },
                [this](Bytes message) { received.push_back(std::move(message)); }, [] {},
                [this](GateError error) { errors.push_back(error); }};
    }
    void activate(bool first) {
        auto &gate = first ? left_gate : right_gate;
        auto &adapter = first ? left_adapter : right_adapter;
        if (!gate) {
            gate = std::make_unique<HandshakeGate>(borrowed(first ? a : b), context(first),
                                                   first ? HandshakeRole::Offerer
                                                         : HandshakeRole::Answerer,
                                                   1024, 4096, adapter, gate_callbacks());
        }
        gate->set_transport_ready(true);
    }
    void relay_description(bool first, rtc::Description description) {
        if (use_signaling) {
            auto &signaling = first ? signaling_a : signaling_b;
            EXPECT_EQ(signaling->local_description(first ? a : b, capability,
                                                   std::move(description), 100, 150, policy),
                      FlowResult::Accepted);
            return;
        }
        const auto text = std::string(description);
        auto &sender = first ? session_a : session_b;
        auto &sender_identity = first ? a : b;
        auto &receiver_box = first ? box_b : box_a;
        const auto kind = first ? SignalingKind::Offer : SignalingKind::Answer;
        auto wire = sender->encode(sender_identity, receiver_box.public_key(), capability, kind,
                                   std::as_bytes(std::span(text)), 100, 150, policy);
        ASSERT_TRUE(std::holds_alternative<Bytes>(wire));
        if (first && defer_offer) {
            pending_offer = std::move(std::get<Bytes>(wire));
            return;
        }
        apply_description(first, std::get<Bytes>(wire));
    }
    void apply_description(bool first, ByteView wire) {
        auto &receiver = first ? session_b : session_a;
        auto &receiver_box = first ? box_b : box_a;
        auto verified = receiver->verify(wire, receiver_box, capability, policy);
        ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(verified));
        const auto &payload = std::get<SignalingEnvelope>(verified).fields.payload;
        const std::string exact(reinterpret_cast<const char *>(payload.data()), payload.size());
        const auto type = first ? rtc::Description::Type::Offer : rtc::Description::Type::Answer;
        (first ? right : left)->set_remote_description(rtc::Description(exact, type));
        auto &buffer = first ? candidates_b : candidates_a;
        ASSERT_TRUE(buffer->mark_description_applied());
        apply_candidates(first);
    }
    void apply_candidates(bool from_first) {
        auto &buffer = from_first ? candidates_b : candidates_a;
        auto &destination = from_first ? right : left;
        while (auto candidate = buffer->pop()) {
            destination->add_remote_candidate(rtc::Candidate(candidate->candidate, candidate->mid));
        }
    }
    void relay_candidate(bool first, rtc::Candidate candidate) {
        if (use_signaling) {
            auto &signaling = first ? signaling_a : signaling_b;
            EXPECT_EQ(signaling->local_candidate(first ? a : b, capability, std::move(candidate),
                                                 100, 150, policy),
                      FlowResult::Accepted);
            ++candidate_count;
            return;
        }
        auto &sender = first ? session_a : session_b;
        auto &receiver = first ? session_b : session_a;
        auto &sender_identity = first ? a : b;
        auto &receiver_box = first ? box_b : box_a;
        auto &buffer = first ? candidates_b : candidates_a;
        const auto candidate_text = candidate.candidate();
        const auto mid = candidate.mid();
        const CandidateLimits limits{4096, 256, 4361};
        auto payload = encode_candidate({candidate_text, mid}, limits);
        ASSERT_TRUE(std::holds_alternative<Bytes>(payload));
        auto wire =
            sender->encode(sender_identity, receiver_box.public_key(), capability,
                           SignalingKind::Candidate, std::get<Bytes>(payload), 100, 150, policy);
        ASSERT_TRUE(std::holds_alternative<Bytes>(wire));
        auto verified = receiver->verify(std::get<Bytes>(wire), receiver_box, capability, policy);
        ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(verified));
        auto queued = buffer->push(std::get<SignalingEnvelope>(verified).fields.payload);
        ASSERT_TRUE(std::holds_alternative<std::monostate>(queued));
        ++candidate_count;
        if (first && pending_offer) {
            EXPECT_FALSE(buffer->pop());
            reordered_candidate = true;
            auto wire = std::move(*pending_offer);
            pending_offer.reset();
            apply_description(true, wire);
        }
        apply_candidates(first);
    }
    NativeLinkCallbacks link_callbacks(bool first) {
        NativeLinkCallbacks out;
        out.on_description = [this, first](rtc::Description description) {
            relay_description(first, std::move(description));
        };
        out.on_candidate = [this, first](rtc::Candidate candidate) {
            auto &destination = first ? right : left;
            if (destination->state() != NativeTransportState::Closed &&
                destination->state() != NativeTransportState::Failed) {
                relay_candidate(first, std::move(candidate));
            }
        };
        out.on_state = [this, first](NativeTransportState state) {
            if (state == NativeTransportState::Ready) {
                activate(first);
            }
        };
        out.on_message = [this, first](Bytes message) {
            auto &gate = first ? left_gate : right_gate;
            ASSERT_TRUE(gate);
            gate->on_message(message);
        };
        out.on_writable = [this, first] {
            auto &gate = first ? left_gate : right_gate;
            if (gate) {
                gate->on_writable();
            }
        };
        out.on_error = [](ConnectError, std::string) {};
        return out;
    }
    void deliver(bool first, Bytes wire) {
        loop.executor()([this, first, wire = std::move(wire)] {
            auto &receiver = first ? signaling_b : signaling_a;
            auto &box = first ? box_b : box_a;
            EXPECT_EQ(receiver->receive(wire, box, capability, policy), FlowResult::Accepted);
        });
    }
    std::function<bool(Bytes)> submit(bool first) {
        return [this, first](Bytes wire) {
            if (first && defer_offer && !bridge_offer_delayed) {
                pending_offer = std::move(wire);
                bridge_offer_delayed = true;
                return true;
            }
            deliver(first, std::move(wire));
            if (first && pending_offer) {
                deliver(first, std::move(*pending_offer));
                pending_offer.reset();
                reordered_candidate = true;
            }
            return true;
        };
    }
    void create() {
        const CandidateLimits candidate_limits{4096, 256, 4361};
        candidates_a =
            std::make_unique<CandidateBuffer>(candidate_limits, MemoryBudget(65536), 65536, 64);
        candidates_b =
            std::make_unique<CandidateBuffer>(candidate_limits, MemoryBudget(65536), 65536, 64);
        SignalingRecipient peer_a{a.public_key(), {std::byte{1}}};
        SignalingRecipient peer_b{b.public_key(), {std::byte{2}}};
        AttemptId attempt{std::byte{3}};
        session_a = std::make_unique<SignalingSession>(peer_a, peer_b, box_a.public_key(),
                                                       box_b.public_key(), attempt,
                                                       HandshakeRole::Offerer, capability, 8);
        session_b = std::make_unique<SignalingSession>(peer_b, peer_a, box_b.public_key(),
                                                       box_a.public_key(), attempt,
                                                       HandshakeRole::Answerer, capability, 8);
        ReliableChannelLimits limits{{4096, 1024}, 4096, 8, 4096, 4};
        auto make = [&](bool first) {
            MemoryBudget budget(64U * 1024U);
            return std::make_unique<NativeReliableLink>(
                rtc::Configuration{}, NativeRelayPolicy::Automatic, limits, budget,
                NativeReceiveLimits{1024}, loop.executor(), link_callbacks(first));
        };
        left = make(true);
        right = make(false);
        left_adapter.link = left.get();
        right_adapter.link = right.get();
        if (use_signaling) {
            NativeSignalBinding left_binding{peer_a,
                                             peer_b,
                                             box_a.public_key(),
                                             box_b.public_key(),
                                             attempt,
                                             HandshakeRole::Offerer};
            NativeSignalBinding right_binding{peer_b,
                                              peer_a,
                                              box_b.public_key(),
                                              box_a.public_key(),
                                              attempt,
                                              HandshakeRole::Answerer};
            signaling_a = std::make_unique<NativeSignaling>(
                left_binding, capability, *left, candidate_limits, MemoryBudget(65536), 65536, 64,
                submit(true), [](SignalingKind) { return true; });
            signaling_b = std::make_unique<NativeSignaling>(
                right_binding, capability, *right, candidate_limits, MemoryBudget(65536), 65536, 64,
                submit(false), [](SignalingKind) { return true; });
        }
    }
};
} // namespace

TEST(NativeHandshakeGate, ExchangesSignedHandshakeAndApplicationMessage) {
    Pair pair;
    pair.create();
    pair.left->start_offer();
    ASSERT_TRUE(pair.loop.until([&] { return pair.ready_count == 2 || !pair.errors.empty(); }));
    ASSERT_TRUE(pair.errors.empty());
    ASSERT_TRUE(pair.left_gate->ready());
    ASSERT_TRUE(pair.right_gate->ready());
    EXPECT_GT(pair.candidate_count, 0);
    EXPECT_EQ(pair.left_gate->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return pair.received.size() == 1; }));
    EXPECT_EQ(pair.received[0], (Bytes{std::byte{0}, std::byte{255}}));
}

TEST(NativeHandshakeGate, NativeSignalingConnectsAndAuthenticatesPeers) {
    Pair pair;
    pair.use_signaling = true;
    pair.create();
    EXPECT_EQ(
        pair.signaling_a->receive(Bytes{std::byte{1}}, pair.box_a, pair.capability, pair.policy),
        FlowResult::Ignored);
    pair.left->start_offer();
    ASSERT_TRUE(pair.loop.until([&] { return pair.ready_count == 2 || !pair.errors.empty(); }));
    ASSERT_TRUE(pair.errors.empty());
    ASSERT_GT(pair.candidate_count, 0);
    ASSERT_EQ(pair.left_gate->send(Bytes{std::byte{8}}), ChannelSend::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return pair.received.size() == 1; }));
    EXPECT_EQ(pair.received.front(), (Bytes{std::byte{8}}));
    pair.signaling_a->close();
    EXPECT_EQ(pair.signaling_a->receive(Bytes{}, pair.box_a, pair.capability, pair.policy),
              FlowResult::Closed);
}

TEST(NativeHandshakeGate, NativeSignalingBuffersCandidateBeforeOffer) {
    Pair pair;
    pair.use_signaling = true;
    pair.defer_offer = true;
    pair.create();
    pair.left->start_offer();
    ASSERT_TRUE(pair.loop.until([&] { return pair.ready_count == 2 || !pair.errors.empty(); }));
    EXPECT_TRUE(pair.errors.empty());
    EXPECT_TRUE(pair.reordered_candidate);
    EXPECT_EQ(pair.ready_count, 2);
}

TEST(NativeHandshakeGate, NativeSignalingFailsWhenCandidatePrecedesLocalDescription) {
    Pair pair;
    pair.use_signaling = true;
    pair.create();
    rtc::Candidate candidate("candidate:1 1 UDP 2122260223 127.0.0.1 5000 typ host", "0");
    EXPECT_EQ(pair.signaling_a->local_candidate(pair.a, pair.capability, std::move(candidate), 100,
                                                150, pair.policy),
              FlowResult::Failed);
    EXPECT_FALSE(pair.signaling_a->handshake_context().has_value());
    EXPECT_EQ(pair.signaling_a->receive(Bytes{}, pair.box_a, pair.capability, pair.policy),
              FlowResult::Failed);
}

TEST(NativeHandshakeGate, RejectsMismatchedSignedContext) {
    Pair pair;
    pair.mismatch = true;
    pair.create();
    pair.left->start_offer();
    ASSERT_TRUE(pair.loop.until([&] { return !pair.errors.empty(); }));
    EXPECT_EQ(pair.errors.front(), GateError::Protocol);
    EXPECT_EQ(pair.ready_count, 0);
    EXPECT_TRUE(pair.received.empty());
}

TEST(NativeHandshakeGate, BuffersSignedCandidateThatArrivesBeforeOffer) {
    Pair pair;
    pair.defer_offer = true;
    pair.create();
    pair.left->start_offer();
    ASSERT_TRUE(pair.loop.until([&] { return pair.ready_count == 2 || !pair.errors.empty(); }));
    EXPECT_TRUE(pair.errors.empty());
    EXPECT_TRUE(pair.reordered_candidate);
    EXPECT_EQ(pair.ready_count, 2);
}
