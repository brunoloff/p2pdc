#include "native_public_incoming.hpp"
#include "terminal_signal.hpp"

#include "../core/timer_producer.hpp"
#include "../security/signaling_limits.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
std::uint64_t utc_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("system clock is before the Unix epoch");
    return static_cast<std::uint64_t>(value);
}

class PublicIncoming final : public IncomingBackend,
                             public std::enable_shared_from_this<PublicIncoming> {
  public:
    PublicIncoming(RecordStore store, IncomingContext initial,
                   std::function<std::optional<IncomingContext>()> context,
                   std::weak_ptr<DiscoveryBackend> lease,
                   std::weak_ptr<DiscoveryBackend> subscription, Executor executor,
                   CallbackGate gate, Scheduler scheduler, IncomingOptions options,
                   IncomingHandlers handlers, std::shared_ptr<IncomingOfferReplay> replay);
    void initialize();
    [[nodiscard]] std::optional<IncomingError> start() override;
    void close() override;
    [[nodiscard]] IncomingState state() const override;
    [[nodiscard]] bool signaling_closed() const override;
    [[nodiscard]] std::optional<Advertisement> peer() const override;
    [[nodiscard]] std::optional<PendingIncoming> peek() const override;
    [[nodiscard]] std::optional<PendingIncoming> take() override;

  private:
    void receive(IncomingOffer offer) noexcept;
    void deliver();
    void fail(IncomingError error) noexcept;
    void tick();
    void cleanup();
    void reject_pending() noexcept;
    void check_cancel(Bytes wire) noexcept;
    void lookup_cancel() noexcept;
    RecordStore store_;
    IncomingContext initial_;
    std::function<std::optional<IncomingContext>()> context_;
    std::weak_ptr<DiscoveryBackend> lease_, subscription_;
    Executor executor_;
    CallbackGate gate_;
    Scheduler scheduler_;
    IncomingOptions options_;
    IncomingHandlers handlers_;
    std::shared_ptr<IncomingOfferReplay> replay_;
    SigningPublicKey signing_key_;
    RecipientBoxPublicKey box_key_;
    std::unique_ptr<TimerProducer> timer_;
    std::unique_ptr<NativeIncomingListener> listener_;
    std::optional<IncomingOffer> offer_;
    std::chrono::steady_clock::time_point deadline_;
    IncomingState state_ = IncomingState::Listening;
};

PublicIncoming::PublicIncoming(RecordStore store, IncomingContext initial,
                               std::function<std::optional<IncomingContext>()> context,
                               std::weak_ptr<DiscoveryBackend> lease,
                               std::weak_ptr<DiscoveryBackend> subscription, Executor executor,
                               CallbackGate gate, Scheduler scheduler, IncomingOptions options,
                               IncomingHandlers handlers,
                               std::shared_ptr<IncomingOfferReplay> replay)
    : store_(std::move(store)), initial_(std::move(initial)), context_(std::move(context)),
      lease_(std::move(lease)), subscription_(std::move(subscription)),
      executor_(std::move(executor)), gate_(gate), scheduler_(std::move(scheduler)),
      options_(options), handlers_(std::move(handlers)), replay_(std::move(replay)),
      signing_key_(initial_.local.endpoint.identity->signing->public_key()),
      box_key_(initial_.local.endpoint.identity->recipient->public_key()) {}

void PublicIncoming::initialize() {
    auto weak = weak_from_this();
    auto executor = executor_;
    executor_ = [weak, executor](auto task) {
        if (auto self = weak.lock())
            executor([self, task = std::move(task)]() mutable { task(); });
    };
    timer_ = std::make_unique<TimerProducer>(executor_, std::move(scheduler_));
}

std::optional<IncomingError> PublicIncoming::start() {
    if (listener_ || state_ != IncomingState::Listening)
        throw std::logic_error("listener already started");
    auto weak = weak_from_this();
    IncomingListenerCallbacks callbacks{[weak](IncomingOffer offer) {
                                            if (auto self = weak.lock())
                                                self->receive(std::move(offer));
                                        },
                                        [weak](IncomingListenerError error) {
                                            if (auto self = weak.lock())
                                                self->fail(incoming_error(error));
                                        },
                                        [weak] {
                                            auto self = weak.lock();
                                            return self && self->state_ == IncomingState::Listening;
                                        },
                                        [weak](Bytes wire) {
                                            if (auto self = weak.lock())
                                                self->check_cancel(std::move(wire));
                                        }};
    auto created =
        NativeIncomingListener::create(store_, initial_, context_, utc_seconds, executor_,
                                       MemoryBudget(1024 * 1024), std::move(callbacks), replay_);
    if (auto error = std::get_if<StoreError>(&created))
        return incoming_error(*error);
    listener_ = std::move(std::get<std::unique_ptr<NativeIncomingListener>>(created));
    tick();
    return std::nullopt;
}

void PublicIncoming::cleanup() {
    if (timer_)
        timer_->close();
    if (listener_) {
        listener_->close();
        listener_.reset();
    }
    initial_.local.endpoint.identity.reset();
    initial_.local.capability.reset();
    initial_.remote.capability.reset();
    initial_.remote.peers.clear();
    context_ = {};
}
void PublicIncoming::close() {
    if (state_ == IncomingState::Closed)
        return;
    if (state_ == IncomingState::Pending && offer_)
        reject_pending();
    state_ = IncomingState::Closed;
    offer_.reset();
    cleanup();
}
void PublicIncoming::reject_pending() noexcept {
    try {
        const auto now = utc_seconds();
        if (now > std::numeric_limits<std::uint64_t>::max() - 50)
            throw std::overflow_error("rejection expiry overflow");
        const auto &remote = offer_->peer;
        SignalingFields fields{SignalingKind::Reject,
                               initial_.local.endpoint.fields.instance_id,
                               {remote.signing_public_key, remote.fields.instance_id},
                               offer_->envelope.fields.attempt_id,
                               0,
                               now,
                               now + 50,
                               {}};
        (void)publish_terminal_signal(
            store_, remote.fields.signaling_inbox, *initial_.local.endpoint.identity->signing,
            std::move(fields), remote.fields.recipient_public_key, *initial_.local.capability,
            {now, 60, initial_.remote.clock_skew, signaling_payload_limit});
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: " << error.what() << '\n';
    }
}
IncomingState PublicIncoming::state() const {
    if (timer_ && !timer_->healthy() && state_ != IncomingState::Closed &&
        state_ != IncomingState::Accepted)
        return IncomingState::Failed;
    return state_;
}
bool PublicIncoming::signaling_closed() const {
    return state_ != IncomingState::Listening && state_ != IncomingState::Pending && !listener_;
}
std::optional<Advertisement> PublicIncoming::peer() const {
    if (state() != IncomingState::Pending || !offer_ ||
        std::chrono::steady_clock::now() >= deadline_)
        return std::nullopt;
    return offer_->peer;
}

std::optional<PendingIncoming> PublicIncoming::peek() const {
    if (state() != IncomingState::Pending || !offer_ ||
        std::chrono::steady_clock::now() >= deadline_)
        return std::nullopt;
    return PendingIncoming{*offer_, initial_.local.endpoint, lease_, subscription_, signing_key_,
                           box_key_};
}

std::optional<PendingIncoming> PublicIncoming::take() {
    if (state() != IncomingState::Pending || !offer_)
        return std::nullopt;
    if (std::chrono::steady_clock::now() >= deadline_) {
        fail(IncomingError::Timeout);
        return std::nullopt;
    }
    PendingIncoming result{
        std::move(*offer_), initial_.local.endpoint, lease_, subscription_, signing_key_, box_key_};
    offer_.reset();
    state_ = IncomingState::Accepted;
    cleanup();
    return result;
}

void PublicIncoming::receive(IncomingOffer offer) noexcept {
    if (state_ != IncomingState::Listening)
        return;
    try {
        offer_ = std::move(offer);
        deadline_ = std::chrono::steady_clock::now() + options_.request_timeout;
        state_ = IncomingState::Pending;
        if (!gate_.post([self = shared_from_this()] { self->deliver(); }))
            fail(IncomingError::Backend);
        else
            lookup_cancel();
    } catch (...) {
        fail(IncomingError::ResourceLimit);
    }
}
void PublicIncoming::deliver() {
    if (state() != IncomingState::Pending || !offer_)
        return;
    if (std::chrono::steady_clock::now() >= deadline_) {
        fail(IncomingError::Timeout);
        return;
    }
    try {
        handlers_.on_request(offer_->peer);
    } catch (...) {
        fail(IncomingError::Callback);
        throw;
    }
}
void PublicIncoming::fail(IncomingError error) noexcept {
    if (state_ == IncomingState::Failed || state_ == IncomingState::Closed ||
        state_ == IncomingState::Accepted)
        return;
    state_ = IncomingState::Failed;
    offer_.reset();
    try {
        (void)gate_.post([self = shared_from_this(), error] {
            if (self->state_ != IncomingState::Failed)
                return;
            self->cleanup();
            if (self->state_ == IncomingState::Failed)
                self->handlers_.on_error(error);
        });
    } catch (...) {
        std::cerr << "p2pdc: incoming error delivery failed; close listener\n";
    }
}
void PublicIncoming::tick() {
    if (state_ != IncomingState::Listening && state_ != IncomingState::Pending)
        return;
    if (state_ == IncomingState::Pending && std::chrono::steady_clock::now() >= deadline_) {
        fail(IncomingError::Timeout);
        return;
    }
    if (!listener_ || !listener_->poll()) {
        fail(IncomingError::Backend);
        return;
    }
    try {
        timer_->arm(options_.poll_interval, [weak = weak_from_this()] {
            if (auto self = weak.lock())
                self->tick();
        });
    } catch (...) {
        fail(IncomingError::Scheduler);
    }
}

void PublicIncoming::check_cancel(Bytes wire) noexcept {
    if (state_ != IncomingState::Pending || !offer_)
        return;
    try {
        const auto now = utc_seconds();
        const auto &local = initial_.local.endpoint;
        SignalingRecipient recipient{signing_key_, local.fields.instance_id};
        auto decoded = decode_signaling(
            wire, *local.identity->recipient, recipient, *initial_.local.capability,
            {now, 60, initial_.remote.clock_skew, signaling_payload_limit});
        const auto *signal = std::get_if<SignalingEnvelope>(&decoded);
        const auto &expected = offer_->envelope;
        if (signal && signal->fields.kind == SignalingKind::Cancel &&
            signal->fields.sequence == cancel_sequence && signal->fields.payload.empty() &&
            signal->sender_signing_key == expected.sender_signing_key &&
            signal->fields.sender_instance == expected.fields.sender_instance &&
            signal->fields.attempt_id == expected.fields.attempt_id)
            fail(IncomingError::Canceled);
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: cancellation verification failed: " << error.what() << '\n';
        fail(IncomingError::Backend);
    }
}

void PublicIncoming::lookup_cancel() noexcept {
    try {
        auto weak = weak_from_this();
        auto executor = executor_;
        auto error = store_.lookup(
            initial_.local.endpoint.fields.signaling_inbox,
            [weak, executor](LookupResult result) {
                try {
                    if (!result.backend_success || result.batch.error)
                        std::cerr << "p2pdc: cancellation catch-up was incomplete\n";
                    executor([weak, result = std::move(result)]() mutable {
                        if (auto self = weak.lock())
                            for (auto &wire : result.batch.values)
                                self->check_cancel(std::move(wire));
                    });
                } catch (const std::exception &error) {
                    std::cerr << "p2pdc: cancellation catch-up delivery failed: " << error.what()
                              << '\n';
                }
            },
            LookupDeduplication::Content);
        if (error)
            std::cerr << "p2pdc: cancellation catch-up could not start\n";
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: cancellation catch-up failed: " << error.what() << '\n';
    }
}
} // namespace

std::shared_ptr<IncomingBackend>
make_public_incoming(RecordStore store, IncomingContext initial,
                     std::function<std::optional<IncomingContext>()> context,
                     std::weak_ptr<DiscoveryBackend> lease,
                     std::weak_ptr<DiscoveryBackend> subscription, Executor executor,
                     CallbackGate gate, Scheduler scheduler, IncomingOptions options,
                     IncomingHandlers handlers, std::shared_ptr<IncomingOfferReplay> replay) {
    if (!context || !executor || !scheduler || !handlers.on_request || !handlers.on_error ||
        !replay || options.request_timeout.count() <= 0 ||
        options.request_timeout > std::chrono::minutes(10) || options.poll_interval.count() < 10 ||
        options.poll_interval > options.request_timeout)
        throw std::invalid_argument("invalid incoming settings");
    auto state = std::make_shared<PublicIncoming>(
        std::move(store), std::move(initial), std::move(context), std::move(lease),
        std::move(subscription), std::move(executor), gate, std::move(scheduler), options,
        std::move(handlers), std::move(replay));
    state->initialize();
    return state;
}
} // namespace p2pdc::detail
