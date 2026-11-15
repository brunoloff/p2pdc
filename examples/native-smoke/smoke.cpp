#include "smoke.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace native_smoke {
Route classify_pair(bool selected, rtc::Candidate::Type local, rtc::Candidate::Type remote) {
    using Type = rtc::Candidate::Type;
    if (!selected) {
        return Route::Unknown;
    }
    if (local == Type::Relayed || remote == Type::Relayed) {
        return Route::TurnRelayed;
    }
    if (local == Type::Unknown || remote == Type::Unknown || local == Type::PeerReflexive ||
        remote == Type::PeerReflexive) {
        return Route::Unknown;
    }
    return Route::Direct;
}

namespace {
rtc::Configuration rtc_config(const Config &config) {
    rtc::Configuration result;
    if (config.mode != Mode::Direct) {
        result.iceTransportPolicy = rtc::TransportPolicy::Relay;
        const auto transport = config.mode == Mode::TurnUdp ? rtc::IceServer::RelayType::TurnUdp
                                                            : rtc::IceServer::RelayType::TurnTcp;
        result.iceServers.emplace_back(config.host, config.port, config.username, config.password,
                                       transport);
    }
    return result;
}

void wire_signaling(rtc::PeerConnection &a, rtc::PeerConnection &b) {
    a.onLocalDescription(
        [&b](rtc::Description description) { b.setRemoteDescription(description); });
    b.onLocalDescription(
        [&a](rtc::Description description) { a.setRemoteDescription(description); });
    a.onLocalCandidate([&b](rtc::Candidate candidate) { b.addRemoteCandidate(candidate); });
    b.onLocalCandidate([&a](rtc::Candidate candidate) { a.addRemoteCandidate(candidate); });
}

bool report_pair(rtc::PeerConnection &peer, bool expect_relay) {
    rtc::Candidate local, remote;
    const bool selected = peer.getSelectedCandidatePair(&local, &remote);
    const bool local_relay = selected && local.type() == rtc::Candidate::Type::Relayed;
    const auto route = classify_pair(selected, local.type(), remote.type());
    const char *name = route == Route::TurnRelayed ? "TurnRelayed"
                       : route == Route::Direct    ? "Direct"
                                                   : "Unknown";
    std::cout << "selected_pair=" << selected << " local=" << local.type()
              << " remote=" << remote.type() << " route=" << name
              << " require_local_relay=" << (expect_relay ? (local_relay ? "met" : "unmet") : "n/a")
              << '\n';
    return route == (expect_relay ? Route::TurnRelayed : Route::Direct);
}

std::shared_ptr<rtc::DataChannel> send_echo(rtc::PeerConnection &peer, std::atomic<bool> &echoed) {
    auto sender = peer.createDataChannel("binary-echo");
    const rtc::binary payload{std::byte{0}, std::byte{255}, std::byte{17}};
    sender->onMessage([&echoed, payload](const rtc::message_variant &message) {
        echoed = std::holds_alternative<rtc::binary>(message) &&
                 std::get<rtc::binary>(message) == payload;
    });
    sender->onOpen([weak = std::weak_ptr<rtc::DataChannel>(sender), payload] {
        if (auto channel = weak.lock()) {
            channel->send(payload);
        }
    });
    return sender;
}
} // namespace

bool run_smoke(const Config &config) {
    const auto settings = rtc_config(config);
    std::mutex receiver_mutex;
    std::shared_ptr<rtc::DataChannel> receiver;
    std::atomic<bool> echoed{false};
    rtc::PeerConnection a(settings), b(settings);
    wire_signaling(a, b);
    b.onDataChannel([&receiver, &receiver_mutex](std::shared_ptr<rtc::DataChannel> channel) {
        channel->onMessage(
            [weak = std::weak_ptr<rtc::DataChannel>(channel)](rtc::message_variant data) {
                if (auto target = weak.lock()) {
                    target->send(data);
                }
            });
        std::lock_guard lock(receiver_mutex);
        receiver = std::move(channel);
    });
    auto sender = send_echo(a, echoed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!echoed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "binary_echo=" << echoed << '\n';
    const bool pair_ok = report_pair(a, config.mode != Mode::Direct);
    sender->close();
    a.close();
    b.close();
    return echoed && pair_ok;
}
} // namespace native_smoke
