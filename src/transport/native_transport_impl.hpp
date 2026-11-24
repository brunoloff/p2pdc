#pragma once

#include "../core/round_trip.hpp"
#include "datagram_support.hpp"
#include "native_config.hpp"
#include "native_datagram_channel.hpp"
#include "native_transport.hpp"
#include "relay_candidate_filter.hpp"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
inline constexpr const char *reliable_channel_label = "p2pdc/reliable/v1";
struct TransportSubmissionGate {
    std::mutex mutex;
    bool accepts = true;
};

struct NativeTransport::Impl : std::enable_shared_from_this<Impl> {
    Executor executor;
    NativeTransportCallbacks callbacks;
    std::size_t max_message_size;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::unique_ptr<NativeDatagramChannel> datagrams;
    NativeRelayPolicy policy;
    NativeTransportState status = NativeTransportState::Connecting;
    RouteTracker tracker;
    RoundTripSmoother round_trip;
    std::atomic<bool> active{true};
    std::shared_ptr<TransportSubmissionGate> submission_gate =
        std::make_shared<TransportSubmissionGate>();
    std::size_t writable_threshold = 0;
    bool stopped = false;
    bool offered = false;
    bool remote_description_set = false;
    bool ready_notified = false;
    bool receive_paused = false;
    std::atomic<bool> drain_scheduled{false};
    std::uint64_t delivery_epoch = 0;

    Impl(rtc::Configuration config, NativeRelayPolicy relay_policy, Executor dispatch,
         NativeTransportCallbacks handlers, NativeReceiveLimits limits)
        : executor(std::move(dispatch)), callbacks(std::move(handlers)),
          max_message_size(limits.max_message_size),
          peer(std::make_shared<rtc::PeerConnection>(
              prepare_native_config(std::move(config), relay_policy))),
          policy(relay_policy) {}

    template <class Action> auto backend(Action action) {
        return [weak = weak_from_this(), gate = submission_gate, dispatch = executor,
                action](auto... arguments) {
            std::lock_guard lock(gate->mutex);
            if (!gate->accepts) {
                return;
            }
            dispatch([weak, action, ... values = std::move(arguments)]() mutable {
                if (auto self = weak.lock(); self && self->active && !self->stopped) {
                    action(*self, std::move(values)...);
                }
            });
        };
    }

    template <class Callback, class... Args> void emit(Callback callback, Args... arguments) {
        if (!callback) {
            return;
        }
        executor([weak = weak_from_this(), epoch = delivery_epoch, callback = std::move(callback),
                  ... values = std::move(arguments)]() mutable {
            if (auto self = weak.lock(); self && self->active && self->delivery_epoch == epoch) {
                callback(std::move(values)...);
            }
        });
    }

    void emit_state(NativeTransportState value) {
        executor([weak = weak_from_this(), epoch = delivery_epoch, value] {
            if (auto self = weak.lock(); self && self->active && self->delivery_epoch == epoch) {
                self->ready_notified = value == NativeTransportState::Ready;
                self->callbacks.on_state(value);
                if (self->ready_notified)
                    self->schedule_drain(); // messages wait until the owner knows it is ready
            }
        });
    }

    void bind_peer() {
        peer->onLocalDescription(backend([](Impl &self, rtc::Description description) {
            if (self.policy == NativeRelayPolicy::RequireLocalRelay)
                (void)retain_relay_candidates(description);
            self.emit(self.callbacks.on_description, std::move(description));
        }));
        peer->onLocalCandidate(backend([](Impl &self, rtc::Candidate candidate) {
            if (self.policy == NativeRelayPolicy::RequireLocalRelay) {
                auto relay = relay_only_candidate(candidate);
                if (!relay)
                    return;
                candidate = std::move(*relay);
            }
            self.emit(self.callbacks.on_candidate, std::move(candidate));
        }));
        peer->onDataChannel(backend([](Impl &self, std::shared_ptr<rtc::DataChannel> incoming) {
            self.attach(std::move(incoming));
        }));
        peer->onStateChange(
            backend([](Impl &self, rtc::PeerConnection::State state) { self.peer_state(state); }));
        peer->onIceStateChange(
            backend([](Impl &self, rtc::PeerConnection::IceState) { self.update_readiness(); }));
    }

    void shutdown() {
        stopped = true;
        ++delivery_epoch;
        tracker.invalidate();
        round_trip.reset();
        if (datagrams)
            datagrams->close();
        if (channel) {
            channel->resetCallbacks();
            channel->close();
            // The backend's reader may wait for room in a full queue; once closed it keeps no more.
            while (channel->receive()) {
            }
        }
        peer->resetCallbacks();
        peer->close();
    }

    void fail(std::string reason, ConnectError category = ConnectError::Transport) {
        if (stopped) {
            return;
        }
        status = NativeTransportState::Failed;
        shutdown();
        emit(callbacks.on_error, category, std::move(reason));
        emit_state(status);
        emit(callbacks.on_route, current_route());
    }

    void attach_datagrams(std::shared_ptr<rtc::DataChannel> incoming) {
        if (offered || !datagrams || !datagrams->adopt(incoming)) {
            incoming->close();
            fail("Unexpected datagram DataChannel", ConnectError::Protocol);
        }
    }

    void attach(std::shared_ptr<rtc::DataChannel> incoming) {
        if (incoming->label() == datagram_channel_label) {
            attach_datagrams(std::move(incoming));
            return;
        }
        const auto reliability = incoming->reliability();
        if (channel || incoming->label() != reliable_channel_label || reliability.unordered ||
            reliability.maxPacketLifeTime || reliability.maxRetransmits) {
            incoming->close();
            fail("Unexpected or unreliable DataChannel", ConnectError::Protocol);
            return;
        }
        channel = std::move(incoming);
        channel->setBufferedAmountLowThreshold(writable_threshold);
        channel->onOpen(backend([](Impl &self) { self.update_readiness(); }));
        channel->onClosed(
            backend([](Impl &self) { self.peer_state(rtc::PeerConnection::State::Closed); }));
        channel->onError(
            backend([](Impl &self, std::string) { self.fail("DataChannel backend error"); }));
        channel->onBufferedAmountLow(
            backend([](Impl &self) { self.emit(self.callbacks.on_writable); }));
        // Messages stay in the backend until drain takes them, so a slow owner holds the peer back.
        channel->onAvailable([weak = weak_from_this(), gate = submission_gate] {
            std::lock_guard lock(gate->mutex);
            if (gate->accepts) {
                if (auto self = weak.lock()) {
                    self->schedule_drain();
                }
            }
        });
        update_readiness();
    }

    void peer_state(rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed) {
            fail("PeerConnection failed; inspect signaling, ICE and TURN configuration");
        } else if (state == rtc::PeerConnection::State::Closed) {
            status = NativeTransportState::Closed;
            shutdown();
            emit_state(status);
            emit(callbacks.on_route, current_route());
        } else {
            update_readiness();
        }
    }

    bool connected() const {
        const auto ice = peer->iceState();
        return peer->state() == rtc::PeerConnection::State::Connected &&
               (ice == rtc::PeerConnection::IceState::Connected ||
                ice == rtc::PeerConnection::IceState::Completed);
    }

    // The SCTP association's round-trip estimate, sampled every few seconds while a path is up.
    void sample_round_trip(bool usable) {
        if (!usable) {
            round_trip.reset();
        } else if (round_trip.due(std::chrono::steady_clock::now())) {
            if (const auto rtt = peer->rtt()) {
                (void)round_trip.sample(*rtt);
            }
        }
    }

    RouteInfo current_route() const {
        auto route = tracker.route();
        route.round_trip_time = round_trip.value();
        return route;
    }

    bool observe_route(bool notify_route) {
        rtc::Candidate local, remote;
        const bool is_connected = connected();
        const bool selected = is_connected && peer->getSelectedCandidatePair(&local, &remote);
        const auto observed = selected ? std::optional{CandidatePair{candidate_kind(local.type()),
                                                                     candidate_kind(remote.type())}}
                                       : std::nullopt;
        const auto pair = usable_route_pair(is_connected, channel && channel->isOpen(), observed);
        tracker.observe(pair, false, std::chrono::steady_clock::now());
        sample_round_trip(pair.has_value());
        if (notify_route) {
            emit(callbacks.on_route, current_route());
        }
        if (is_connected && channel && channel->isOpen() &&
            policy == NativeRelayPolicy::RequireLocalRelay &&
            (!pair || pair->local != CandidateKind::Relayed)) {
            fail("RequireLocalRelay selected local candidate is non-relayed or unknown");
            return false;
        }
        return pair.has_value();
    }

    void update_readiness(bool notify_route = true) {
        const bool selected = observe_route(notify_route);
        if (stopped) {
            return;
        }
        const bool is_connected = connected();
        const bool ready = is_connected && channel && channel->isOpen() &&
                           (policy == NativeRelayPolicy::Automatic || selected);
        const bool previously_ready =
            status == NativeTransportState::Ready || status == NativeTransportState::Suspended;
        const auto next = ready ? NativeTransportState::Ready
                                : (previously_ready ? NativeTransportState::Suspended
                                                    : NativeTransportState::Connecting);
        if (status != next) {
            status = next;
            ready_notified = false;
            ++delivery_epoch;
            emit_state(status);
            emit(callbacks.on_route, current_route());
        }
    }

    void schedule_drain() {
        if (drain_scheduled.exchange(true)) {
            return;
        }
        executor([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && self->active && !self->stopped) {
                self->drain();
            }
        });
    }
    void resume_receive() {
        if (receive_paused && active && !stopped) {
            receive_paused = false;
            schedule_drain();
        }
    }
    // Delivers up to inbound_frames_per_pass messages while the owner has room for them.
    void drain() {
        drain_scheduled = false;
        for (unsigned count = 0; count < inbound_frames_per_pass; ++count) {
            update_readiness(false);
            if (stopped || !active || status != NativeTransportState::Ready || !ready_notified) {
                return; // drained again by the next Ready notification
            }
            if (callbacks.can_receive && !callbacks.can_receive()) {
                receive_paused = true;
                return;
            }
            auto message = channel->receive();
            if (!message) {
                return; // onAvailable schedules the next drain
            }
            auto *binary = std::get_if<rtc::binary>(&*message);
            if (!binary) {
                fail("Text received on binary-only transport", ConnectError::Protocol);
                return;
            }
            if (binary->size() > max_message_size) {
                fail("Inbound message exceeds receive limits", ConnectError::ResourceLimit);
                return;
            }
            callbacks.on_binary(std::move(*binary));
        }
        schedule_drain();
    }
};

} // namespace p2pdc::detail
