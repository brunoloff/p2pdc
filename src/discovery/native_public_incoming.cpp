#include "public_incoming_impl.hpp"
#include "terminal_signal.hpp"

#include "../core/timer_producer.hpp"
#include "../security/signaling_limits.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
static std::uint64_t utc_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("system clock is before the Unix epoch");
    return static_cast<std::uint64_t>(value);
}

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
      box_key_(initial_.local.endpoint.identity->recipient->public_key()),
      offers_(options.persistent ? options.max_pending_requests : 1, 1048576) {}

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
                                        [weak](const Bytes &wire) {
                                            if (auto self = weak.lock())
                                                self->check_cancel(wire);
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
    if (state_ == IncomingState::Pending && pending())
        reject_pending(*pending_id_);
    if (options_.persistent)
        while (auto id = offers_.first()) {
            reject_pending(*id);
            (void)offers_.take(*id);
        }
    state_ = IncomingState::Closed;
    clear_pending();
    cleanup();
}
const PendingOffers::Entry *PublicIncoming::pending() const {
    return pending_id_ ? offers_.find(*pending_id_) : nullptr;
}
void PublicIncoming::clear_pending() {
    if (pending_id_)
        (void)offers_.take(*pending_id_);
    pending_id_.reset();
}
void PublicIncoming::reject_pending(IncomingRequestId id) noexcept {
    if (const auto *entry = offers_.find(id))
        reject_offer(entry->offer);
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
    const auto *entry = pending();
    if (state() != IncomingState::Pending || !entry ||
        std::chrono::steady_clock::now() >= entry->deadline)
        return std::nullopt;
    return entry->offer.peer;
}
std::optional<IncomingRequestId> PublicIncoming::request_id() const {
    return peer() ? pending_id_ : std::nullopt;
}

std::optional<PendingIncoming> PublicIncoming::peek() const {
    const auto *entry = pending();
    if (state() != IncomingState::Pending || !entry ||
        std::chrono::steady_clock::now() >= entry->deadline)
        return std::nullopt;
    return PendingIncoming{
        entry->offer, initial_.local.endpoint, lease_, subscription_, signing_key_, box_key_};
}

std::optional<PendingIncoming> PublicIncoming::take() {
    const auto *entry = pending();
    if (state() != IncomingState::Pending || !entry)
        return std::nullopt;
    if (std::chrono::steady_clock::now() >= entry->deadline) {
        fail(IncomingError::Timeout);
        return std::nullopt;
    }
    auto taken = offers_.take(*pending_id_);
    pending_id_.reset();
    PendingIncoming result{std::move(taken->offer),
                           initial_.local.endpoint,
                           lease_,
                           subscription_,
                           signing_key_,
                           box_key_};
    state_ = IncomingState::Accepted;
    cleanup();
    return result;
}

std::optional<PendingIncoming> PublicIncoming::peek_request(std::optional<std::uint64_t> id) const {
    if (!options_.persistent)
        return !id || id == pending_id_ ? peek() : std::nullopt;
    const auto *entry = id ? offers_.find(*id) : nullptr;
    if (state() != IncomingState::Listening || !entry ||
        std::chrono::steady_clock::now() >= entry->deadline)
        return std::nullopt;
    return PendingIncoming{
        entry->offer, initial_.local.endpoint, lease_, subscription_, signing_key_, box_key_};
}
std::optional<PendingIncoming> PublicIncoming::take_request(std::optional<std::uint64_t> id) {
    if (!options_.persistent)
        return !id || id == pending_id_ ? take() : std::nullopt;
    if (!peek_request(id))
        return std::nullopt;
    auto entry = offers_.take(*id);
    return PendingIncoming{std::move(entry->offer),
                           initial_.local.endpoint,
                           lease_,
                           subscription_,
                           signing_key_,
                           box_key_};
}
void PublicIncoming::reject_request(std::optional<std::uint64_t> id) {
    if (options_.persistent && id && peek_request(id)) {
        reject_pending(*id);
        (void)offers_.take(*id);
    } else if (!options_.persistent && (!id || id == pending_id_))
        close();
}

void PublicIncoming::receive(IncomingOffer offer) noexcept {
    if (state_ != IncomingState::Listening)
        return;
    try {
        auto id = offers_.add(std::move(offer),
                              std::chrono::steady_clock::now() + options_.request_timeout);
        if (!id) {
            if (options_.persistent) {
                reject_offer(offer);
                report_overflow();
                return;
            }
            fail(IncomingError::ResourceLimit);
            return;
        }
        if (!options_.persistent) {
            pending_id_ = *id;
            state_ = IncomingState::Pending;
        }
        if (!gate_.post([self = shared_from_this(), id = *id] { self->deliver(id); }))
            fail(IncomingError::Backend);
        else
            lookup_cancel();
    } catch (...) {
        fail(IncomingError::ResourceLimit);
    }
}
void PublicIncoming::deliver(IncomingRequestId id) {
    const auto *entry = offers_.find(id);
    if ((options_.persistent ? state() != IncomingState::Listening
                             : state() != IncomingState::Pending) ||
        !entry)
        return;
    if (std::chrono::steady_clock::now() >= entry->deadline) {
        if (options_.persistent)
            end_request(id, IncomingError::Timeout);
        else
            fail(IncomingError::Timeout);
        return;
    }
    try {
        if (options_.persistent)
            handlers_.on_request_identified(id, entry->offer.peer);
        else
            handlers_.on_request(entry->offer.peer);
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
    clear_pending();
    while (auto id = offers_.first())
        (void)offers_.take(*id);
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
    while (options_.persistent && state_ == IncomingState::Listening) {
        auto id = offers_.first_expired(std::chrono::steady_clock::now());
        if (!id)
            break;
        end_request(*id, IncomingError::Timeout);
    }
    if (state_ == IncomingState::Pending &&
        offers_.first_expired(std::chrono::steady_clock::now())) {
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

void PublicIncoming::check_cancel(const Bytes &wire) noexcept {
    const auto *entry = pending();
    if (options_.persistent ? state_ != IncomingState::Listening || offers_.size() == 0
                            : state_ != IncomingState::Pending || !entry)
        return;
    try {
        const auto now = utc_seconds();
        const auto &local = initial_.local.endpoint;
        SignalingRecipient recipient{signing_key_, local.fields.instance_id};
        auto decoded = decode_signaling(
            wire, *local.identity->recipient, recipient, *initial_.local.capability,
            {now, 60, initial_.remote.clock_skew, signaling_payload_limit});
        const auto *signal = std::get_if<SignalingEnvelope>(&decoded);
        if (signal && signal->fields.kind == SignalingKind::Cancel &&
            signal->fields.sequence == cancel_sequence && signal->fields.payload.empty()) {
            auto id = offers_.find_attempt(*signal);
            if (id && options_.persistent)
                end_request(*id, IncomingError::Canceled);
            else if (id)
                fail(IncomingError::Canceled);
        }
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: cancellation verification failed: " << error.what() << '\n';
        fail(IncomingError::Backend);
    }
}

void PublicIncoming::end_request(IncomingRequestId id, IncomingError error) {
    if (!offers_.take(id))
        return;
    if (handlers_.on_request_ended && !gate_.post([self = shared_from_this(), id, error] {
            if (self->state_ == IncomingState::Listening && self->handlers_.on_request_ended)
                self->handlers_.on_request_ended(id, error);
        }))
        fail(IncomingError::Backend);
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
                                self->check_cancel(wire);
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

std::shared_ptr<IncomingBackend>
make_public_incoming(RecordStore store, IncomingContext initial,
                     std::function<std::optional<IncomingContext>()> context,
                     std::weak_ptr<DiscoveryBackend> lease,
                     std::weak_ptr<DiscoveryBackend> subscription, Executor executor,
                     CallbackGate gate, Scheduler scheduler, IncomingOptions options,
                     IncomingHandlers handlers, std::shared_ptr<IncomingOfferReplay> replay) {
    if (!context || !executor || !scheduler ||
        (options.persistent ? !handlers.on_request_identified : !handlers.on_request) ||
        !handlers.on_error || !options.max_pending_requests || options.max_pending_requests > 128 ||
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
