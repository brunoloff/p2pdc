#include "../src/transport/native_reliable_link.hpp"

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
std::string hex(const std::string &value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : value) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
std::string unhex(const std::string &value) {
    if (value.size() % 2)
        throw std::runtime_error("invalid hex");
    std::string out;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const auto digit = [](char c) -> unsigned {
            if (c >= '0' && c <= '9')
                return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f')
                return static_cast<unsigned>(c - 'a' + 10);
            throw std::runtime_error("invalid hex digit");
        };
        out += static_cast<char>((digit(value[i]) << 4) | digit(value[i + 1]));
    }
    return out;
}
class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        };
    }
    void drain() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (queue_.empty())
                    return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};
struct Peer {
    Loop loop;
    std::unique_ptr<NativeReliableLink> link;
    bool sent = false, received = false, done = false, notified = false, stop = false;
    std::string error;
    NativeLinkCallbacks callbacks();
    void input(const std::string &line);
    void send();
};
NativeLinkCallbacks Peer::callbacks() {
    NativeLinkCallbacks out;
    out.on_description = [](rtc::Description d) {
        std::cout << "D " << d.typeString() << ' ' << hex(std::string(d)) << std::endl;
    };
    out.on_candidate = [](rtc::Candidate c) {
        std::cout << "C " << hex(c.mid()) << ' ' << hex(c.candidate()) << std::endl;
    };
    out.on_state = [this](NativeTransportState state) {
        if (state == NativeTransportState::Ready && !sent)
            send();
        if (state == NativeTransportState::Failed)
            error = "transport failed";
    };
    out.on_route = [](RouteInfo) {};
    out.on_error = [this](std::string value) { error = std::move(value); };
    out.on_writable = [] {};
    out.on_message = [this](Bytes value) {
        if (received) {
            error = "duplicate message";
            return;
        }
        if (value.size() != 70000) {
            error = "message length mismatch";
            return;
        }
        for (std::size_t i = 0; i < value.size(); ++i)
            if (value[i] != static_cast<std::byte>(i % 251)) {
                error = "message content mismatch";
                return;
            }
        received = true;
    };
    return out;
}
void Peer::send() {
    Bytes value(90000);
    for (std::size_t i = 0; i < value.size(); ++i)
        value[i] = static_cast<std::byte>((i * 7) % 256);
    if (link->send(value) != ChannelSend::Accepted) {
        error = "send rejected";
        return;
    }
    sent = true;
}
void Peer::input(const std::string &line) {
    std::istringstream in(line);
    std::string kind, first, second;
    in >> kind >> first >> second;
    if (kind == "D" && first == "answer" && !second.empty())
        link->set_remote_description(rtc::Description(unhex(second), first));
    else if (kind == "C" && !first.empty() && !second.empty())
        link->add_remote_candidate(rtc::Candidate(unhex(second), unhex(first)));
    else if (kind == "DONE")
        done = true;
    else if (kind == "STOP" && notified)
        stop = true;
    else
        throw std::runtime_error("invalid signaling input");
}
} // namespace

int main() {
    try {
        Peer peer;
        MemoryBudget budget(4 * 1024 * 1024);
        const ReliableChannelLimits limits{{262145, 1024}, 2097152, 32, 4096, 4};
        NativeReceiveLimits inbound{budget, 1048576, 128, 1024};
        peer.link = std::make_unique<NativeReliableLink>(
            rtc::Configuration{}, NativeRelayPolicy::Automatic, limits, budget, inbound,
            peer.loop.executor(), peer.callbacks());
        peer.link->start_offer();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::string pending;
        while (std::chrono::steady_clock::now() < deadline && peer.error.empty()) {
            peer.loop.drain();
            if (peer.sent && peer.received && peer.done && !peer.notified) {
                std::cout << "PASS" << std::endl;
                peer.notified = true;
            }
            if (peer.stop)
                return 0;
            pollfd input{STDIN_FILENO, POLLIN, 0};
            if (poll(&input, 1, 5) > 0 && (input.revents & POLLIN)) {
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
            if (input.revents & (POLLERR | POLLHUP | POLLNVAL))
                throw std::runtime_error("signaling closed");
        }
        throw std::runtime_error(peer.error.empty() ? "timeout" : peer.error);
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
