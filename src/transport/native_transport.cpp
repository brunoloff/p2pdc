#include "native_transport.hpp"
#include "inbound_queue.hpp"
#include "native_config.hpp"
#include "relay_candidate_filter.hpp"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr const char *channel_label = "p2pdc/reliable/v1";
struct SubmissionGate {
    std::mutex mutex;
    bool accepts = true;
};
} // namespace

struct NativeTransport::Impl : std::enable_shared_from_this<Impl> {
    Executor executor;
    NativeTransportCallbacks callbacks;
    InboundQueue inbound;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    NativeRelayPolicy policy;
    NativeTransportState status = NativeTransportState::Connecting;
    RouteTracker tracker;
    std::atomic<bool> active{true};
    std::shared_ptr<SubmissionGate> submission_gate = std::make_shared<SubmissionGate>();
    std::size_t writable_threshold = 0;
    bool stopped = false;
    bool offered = false;
    bool remote_description_set = false;
    bool ready_notified = false;
    std::uint64_t delivery_epoch = 0;

    Impl(rtc::Configuration config, NativeRelayPolicy relay_policy, Executor dispatch,
         NativeTransportCallbacks handlers, NativeReceiveLimits limits)
        : executor(std::move(dispatch)), callbacks(std::move(handlers)),
          inbound(std::move(limits.budget), limits.max_bytes, limits.max_messages,
                  limits.max_message_size),
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
        inbound.close();
        ++delivery_epoch;
        tracker.invalidate();
        if (channel) {
            channel->resetCallbacks();
            channel->close();
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
        emit(callbacks.on_route, tracker.route());
    }

    void attach(std::shared_ptr<rtc::DataChannel> incoming) {
        const auto reliability = incoming->reliability();
        if (channel || incoming->label() != channel_label || reliability.unordered ||
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
        channel->onMessage(
            [weak = weak_from_this(), gate = submission_gate](rtc::message_variant message) {
                std::lock_guard lock(gate->mutex);
                if (gate->accepts) {
                    if (auto self = weak.lock()) {
                        self->enqueue(std::move(message));
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
            emit(callbacks.on_route, tracker.route());
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

    bool observe_route(bool notify_route) {
        rtc::Candidate local, remote;
        const bool is_connected = connected();
        const bool selected = is_connected && peer->getSelectedCandidatePair(&local, &remote);
        const auto observed = selected ? std::optional{CandidatePair{candidate_kind(local.type()),
                                                                     candidate_kind(remote.type())}}
                                       : std::nullopt;
        const auto pair = usable_route_pair(is_connected, channel && channel->isOpen(), observed);
        tracker.observe(pair, false, std::chrono::steady_clock::now());
        if (notify_route) {
            emit(callbacks.on_route, tracker.route());
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
            emit(callbacks.on_route, tracker.route());
        }
    }

    void enqueue(rtc::message_variant message) {
        auto *binary = std::get_if<rtc::binary>(&message);
        const auto result =
            binary ? inbound.push(std::move(*binary)) : inbound.reject_invalid_type();
        if (result.schedule_drain) {
            schedule_drain();
        }
    }

    void schedule_drain() {
        executor([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && self->active && !self->stopped) {
                self->drain();
            }
        });
    }

    void drain() {
        for (unsigned count = 0; count < 16; ++count) {
            update_readiness(false);
            if (stopped || !active) {
                return;
            }
            if (status == NativeTransportState::Ready && !ready_notified) {
                schedule_drain();
                return;
            }
            auto item = inbound.next();
            if (auto *message = std::get_if<InboundMessage>(&item)) {
                if (status != NativeTransportState::Ready) {
                    fail("Binary received before transport readiness", ConnectError::Protocol);
                    return;
                }
                callbacks.on_binary(std::move(message->payload));
                if (stopped || !active) {
                    return;
                }
            } else {
                const auto result = std::get<InboundStatus>(item);
                if (result != InboundStatus::Idle && result != InboundStatus::Closed) {
                    fail(result == InboundStatus::InvalidType
                             ? "Text received on binary-only transport"
                             : "Inbound message exceeds receive limits",
                         result == InboundStatus::InvalidType ? ConnectError::Protocol
                                                              : ConnectError::ResourceLimit);
                }
                return;
            }
        }
        schedule_drain();
    }
};

NativeTransport::NativeTransport(rtc::Configuration config, NativeRelayPolicy policy,
                                 Executor executor, NativeTransportCallbacks callbacks,
                                 NativeReceiveLimits limits) {
    if (!executor || !callbacks.on_description || !callbacks.on_candidate || !callbacks.on_binary ||
        !callbacks.on_state || !callbacks.on_error) {
        throw std::invalid_argument(
            "Native transport requires executor and signaling/data/state/error callbacks");
    }
    impl_ = std::make_shared<Impl>(std::move(config), policy, std::move(executor),
                                   std::move(callbacks), std::move(limits));
    impl_->bind_peer();
}

NativeTransport::~NativeTransport() { close(); }

void NativeTransport::start_offer() {
    if (impl_->stopped || impl_->offered || impl_->channel || impl_->peer->remoteDescription()) {
        throw std::logic_error("Offer cannot start on this transport state");
    }
    impl_->offered = true;
    impl_->attach(impl_->peer->createDataChannel(channel_label));
    impl_->peer->setLocalDescription(rtc::Description::Type::Offer);
}

void NativeTransport::set_remote_description(rtc::Description description) {
    if (impl_->stopped) {
        throw std::logic_error("Transport is closed");
    }
    if (description.type() != rtc::Description::Type::Offer &&
        description.type() != rtc::Description::Type::Answer) {
        throw std::invalid_argument("Only offer/answer descriptions are supported");
    }
    const bool offer = description.type() == rtc::Description::Type::Offer;
    if (impl_->remote_description_set || offer == impl_->offered) {
        throw std::logic_error("Unexpected description or unsupported renegotiation");
    }
    impl_->peer->setRemoteDescription(std::move(description));
    impl_->remote_description_set = true;
    if (offer) {
        impl_->peer->setLocalDescription(rtc::Description::Type::Answer);
    }
}

void NativeTransport::add_remote_candidate(rtc::Candidate candidate) {
    if (impl_->stopped) {
        throw std::logic_error("Transport is closed");
    }
    impl_->peer->addRemoteCandidate(std::move(candidate));
}

NativeSendResult NativeTransport::send(ByteView bytes) {
    if (impl_->stopped) {
        return NativeSendResult::NotReady;
    }
    impl_->update_readiness(false);
    if (impl_->status != NativeTransportState::Ready) {
        return NativeSendResult::NotReady;
    }
    if (bytes.size() > impl_->channel->maxMessageSize()) {
        return NativeSendResult::TooLarge;
    }
    try {
        // With the required patched backend, false means accepted but buffered.
        impl_->channel->send(rtc::binary(bytes.begin(), bytes.end()));
        return NativeSendResult::Accepted;
    } catch (const std::exception &error) {
        impl_->fail(std::string("Binary send failed in DataChannel backend: ") + error.what());
        return NativeSendResult::Failed;
    }
}

void NativeTransport::set_writable_threshold(std::size_t bytes) {
    impl_->writable_threshold = bytes;
    if (impl_->channel)
        impl_->channel->setBufferedAmountLowThreshold(bytes);
}

std::size_t NativeTransport::writable_threshold() const { return impl_->writable_threshold; }

std::size_t NativeTransport::buffered_amount() const {
    return impl_->channel ? impl_->channel->bufferedAmount() : 0;
}
std::size_t NativeTransport::max_message_size() const {
    return impl_->channel ? impl_->channel->maxMessageSize() : 0;
}
NativeTransportState NativeTransport::state() const { return impl_->status; }
RouteInfo NativeTransport::poll_route() {
    if (!impl_->stopped) {
        impl_->update_readiness();
    }
    return impl_->tracker.route();
}
void NativeTransport::close() {
    {
        std::lock_guard lock(impl_->submission_gate->mutex);
        if (!impl_->active.exchange(false)) {
            return;
        }
        impl_->submission_gate->accepts = false;
    }
    impl_->status = NativeTransportState::Closed;
    impl_->shutdown();
}
} // namespace p2pdc::detail
