#include "../src/security/secure_attempt.hpp"
#include "../src/transport/native_transport.hpp"
#include "borrowed.hpp"

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
std::string hex(ByteView value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : value) {
        auto c = std::to_integer<unsigned>(byte);
        result += digits[c >> 4U];
        result += digits[c & 15U];
    }
    return result;
}
Bytes unhex(const std::string &value) {
    if (value.size() % 2 || value.size() > 65536)
        throw std::runtime_error("invalid wire size");
    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("invalid wire hex");
    };
    Bytes result;
    for (std::size_t i = 0; i < value.size(); i += 2)
        result.push_back(static_cast<std::byte>((digit(value[i]) << 4U) | digit(value[i + 1])));
    return result;
}
std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
SignalingPolicy policy() { return {now(), 60, 5, 32768 - signaling_wire_overhead}; }
class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    void drain() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};
struct Peer final : ReliableSink {
    Identity identity = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    RecipientKeyPair box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    Identity remote = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    RecipientKeyPair remote_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    Bytes capability = Bytes(32, std::byte{5});
    Loop loop;
    std::unique_ptr<NativeTransport> transport;
    std::unique_ptr<SecureAttempt> attempt;
    bool sent = false, received = false, done = false, notified = false, stop = false,
         described = false;
    std::string error;
    std::vector<rtc::Candidate> early;
    [[nodiscard]] std::size_t buffered_amount() const override {
        return transport->buffered_amount();
    }
    [[nodiscard]] SinkResult send(ByteView frame) override;
    NativeTransportCallbacks transport_callbacks();
    SecureAttemptCallbacks attempt_callbacks();
    void local_description(rtc::Description description);
    void local_candidate(rtc::Candidate candidate);
    bool install_description(ByteView payload);
    bool install_candidate(CandidateView candidate);
    void send_message();
    void on_message(Bytes value);
    void input(const std::string &line);
    void create();
};
SinkResult Peer::send(ByteView frame) {
    try {
        switch (transport->send(frame)) {
        case NativeSendResult::Accepted:
            return SinkResult::Accepted;
        case NativeSendResult::NotReady:
            return SinkResult::NotReady;
        default:
            return SinkResult::Failed;
        }
    } catch (const std::exception &) {
        return SinkResult::Failed;
    }
}
NativeTransportCallbacks Peer::transport_callbacks() {
    NativeTransportCallbacks out;
    out.on_description = [this](rtc::Description value) { local_description(std::move(value)); };
    out.on_candidate = [this](rtc::Candidate value) { local_candidate(std::move(value)); };
    out.on_state = [this](NativeTransportState value) {
        if (value == NativeTransportState::Failed)
            error = "native transport failed";
        else if (value == NativeTransportState::Ready && transport->max_message_size() < 1024)
            error = "negotiated frame limit too small";
        else
            attempt->set_transport_ready(value == NativeTransportState::Ready);
    };
    out.on_binary = [this](Bytes value) { attempt->on_frame(value); };
    out.on_route = [](RouteInfo) {};
    out.on_error = [this](std::string value) { error = std::move(value); };
    out.on_writable = [this] { attempt->wake(); };
    return out;
}
SecureAttemptCallbacks Peer::attempt_callbacks() {
    SignalingFlowCallbacks signaling{
        [](Bytes wire) {
            std::cout << "W " << hex(wire) << std::endl;
            return true;
        },
        [this](ByteView value) { return install_description(value); },
        [this](CandidateView value) { return install_candidate(value); },
        [](SignalingKind) { return true; }};
    return {std::move(signaling),
            [this](std::uint32_t) { loop.executor()([this] { send_message(); }); },
            [this](Bytes value) { on_message(std::move(value)); }, [] {},
            [this](AttemptFailure) { error = "authenticated attempt failed"; }};
}
void Peer::local_description(rtc::Description description) {
    const auto text = std::string(description);
    auto p = policy();
    if (attempt->local_description(std::as_bytes(std::span(text)), p.now, p.now + 60, p) !=
        FlowResult::Accepted) {
        error = "local description failed";
        return;
    }
    described = true;
    for (auto &candidate : early)
        local_candidate(std::move(candidate));
    early.clear();
}
void Peer::local_candidate(rtc::Candidate candidate) {
    if (!described) {
        if (early.size() >= 63) {
            error = "candidate limit";
            return;
        }
        early.push_back(std::move(candidate));
        return;
    }
    auto p = policy();
    const auto text = candidate.candidate(), mid = candidate.mid();
    if (mid.empty() ||
        attempt->local_candidate({text, mid}, p.now, p.now + 60, p) != FlowResult::Accepted)
        error = "local candidate failed";
}
bool Peer::install_description(ByteView payload) {
    try {
        const std::string text(reinterpret_cast<const char *>(payload.data()), payload.size());
        transport->set_remote_description(rtc::Description(text, rtc::Description::Type::Answer));
        loop.executor()([this] {
            if (attempt->description_applied(true) != FlowResult::Accepted)
                error = "description completion failed";
        });
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
bool Peer::install_candidate(CandidateView candidate) {
    try {
        const auto operation = attempt->candidate_operation();
        if (!operation || candidate.mid.empty())
            return false;
        transport->add_remote_candidate(
            rtc::Candidate(std::string(candidate.candidate), std::string(candidate.mid)));
        loop.executor()([this, operation] {
            if (attempt->candidate_applied(*operation, true) != FlowResult::Accepted)
                error = "candidate completion failed";
        });
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
void Peer::send_message() {
    if (sent) {
        error = "duplicate readiness";
        return;
    }
    Bytes value(90000);
    for (std::size_t i = 0; i < value.size(); ++i)
        value[i] = static_cast<std::byte>((i * 7) % 256);
    if (attempt->send(value) != ChannelSend::Accepted) {
        error = "send rejected";
        return;
    }
    sent = true;
}
void Peer::on_message(Bytes value) {
    if (received || value.size() != 70000) {
        error = "message length or duplicate";
        return;
    }
    for (std::size_t i = 0; i < value.size(); ++i)
        if (value[i] != static_cast<std::byte>(i % 251)) {
            error = "message content mismatch";
            return;
        }
    received = true;
}
void Peer::input(const std::string &line) {
    std::istringstream input(line);
    std::string kind, body;
    input >> kind >> body;
    if (kind == "W" && !body.empty()) {
        if (attempt->receive_signaling(unhex(body), policy()) != FlowResult::Accepted)
            throw std::runtime_error("signaling rejected");
    } else if (kind == "DONE")
        done = true;
    else if (kind == "STOP" && notified)
        stop = true;
    else
        throw std::runtime_error("invalid signaling input");
}
void Peer::create() {
    SignalingInstanceId local{}, other{};
    local.fill(std::byte{1});
    other.fill(std::byte{2});
    AttemptId id{};
    id.fill(std::byte{3});
    AsyncSignalingBinding binding{{identity.public_key(), local},
                                  {remote.public_key(), other},
                                  box.public_key(),
                                  remote_box.public_key(),
                                  id,
                                  HandshakeRole::Offerer};
    MemoryBudget budget(4194304);
    rtc::Configuration config;
    config.maxMessageSize = 1024;
    transport = std::make_unique<NativeTransport>(config, NativeRelayPolicy::Automatic,
                                                  loop.executor(), transport_callbacks(),
                                                  NativeReceiveLimits{budget, 1048576, 128, 1024});
    attempt = std::make_unique<SecureAttempt>(
        binding, capability, borrowed(identity), box, CandidateLimits{4096, 256, 4361}, budget,
        ReliableChannelLimits{{262145, 1024}, 2097152, 32, 524288, 4}, 262144, *this,
        loop.executor(), attempt_callbacks());
    transport->start_offer();
}
void read_input(Peer &peer, std::string &pending) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 5) > 0 &&
        (static_cast<unsigned>(input.revents) & static_cast<unsigned>(POLLIN))) {
        char bytes[4096];
        const auto count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count <= 0)
            throw std::runtime_error("signaling closed");
        pending.append(bytes, static_cast<std::size_t>(count));
        if (pending.size() > 131072)
            throw std::runtime_error("signaling limit exceeded");
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            peer.input(pending.substr(0, end));
            pending.erase(0, end + 1);
        }
    }
    if (static_cast<unsigned>(input.revents) &
        (static_cast<unsigned>(POLLERR) | static_cast<unsigned>(POLLHUP) |
         static_cast<unsigned>(POLLNVAL)))
        throw std::runtime_error("signaling closed");
}
} // namespace

int main() {
    try {
        Peer peer;
        peer.create();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::string pending;
        while (std::chrono::steady_clock::now() < deadline && peer.error.empty()) {
            peer.loop.drain();
            if (!peer.error.empty())
                break;
            if (peer.sent && peer.received && peer.done && !peer.notified) {
                std::cout << "PASS" << std::endl;
                peer.notified = true;
            }
            if (peer.stop)
                return 0;
            read_input(peer, pending);
        }
        throw std::runtime_error(peer.error.empty() ? "timeout" : peer.error);
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
