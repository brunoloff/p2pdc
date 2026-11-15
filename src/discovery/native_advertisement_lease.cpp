#include "native_advertisement_lease.hpp"
#include "discovery_window.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
bool nonzero(ByteView bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](auto value) { return value != std::byte{}; });
}
} // namespace

struct NativeAdvertisementLease::State : std::enable_shared_from_this<State> {
    std::shared_ptr<const Identity> identity;
    std::shared_ptr<const Bytes> capability;
    AdvertisementLeaseConfig config;
    std::function<std::uint64_t()> now;
    AdvertisementLeaseCallbacks callbacks;
    RecordStore store;
    MemoryBudget budget;
    Executor executor;
    std::unique_ptr<NativeOutbox> outbox;
    std::optional<std::uint64_t> window;
    std::optional<std::uint64_t> last_now;
    std::optional<AdvertisementFields> published_fields;
    std::uint64_t next_refresh = 0;
    std::uint64_t pending_expiry = 0;
    std::uint64_t pending_sequence = 0;
    std::size_t publications = 0;
    bool active = true;
    bool healthy = true;
    bool pending = false;
    bool paused = false;
    bool exhausted = false;

    State(std::shared_ptr<const Identity> id, std::shared_ptr<const Bytes> secret,
          AdvertisementLeaseConfig settings, std::function<std::uint64_t()> clock,
          AdvertisementLeaseCallbacks handlers, RecordStore publisher_store,
          MemoryBudget publisher_budget, Executor publisher_executor)
        : identity(std::move(id)), capability(std::move(secret)), config(settings),
          now(std::move(clock)), callbacks(std::move(handlers)), store(std::move(publisher_store)),
          budget(std::move(publisher_budget)), executor(std::move(publisher_executor)) {}
    void fail(AdvertisementLeaseFailure error);
    void complete();
    [[nodiscard]] std::optional<std::uint64_t> time();
    [[nodiscard]] AdvertisementLeaseTick publish(std::uint64_t current);
    [[nodiscard]] bool prepare_window(std::uint64_t current);
    [[nodiscard]] bool can_publish(std::uint64_t current);
    [[nodiscard]] OutboxCallbacks handlers();
};

void NativeAdvertisementLease::State::fail(AdvertisementLeaseFailure error) {
    if (active && healthy) {
        healthy = false;
        callbacks.on_error(error);
    }
}

void NativeAdvertisementLease::State::complete() {
    try {
        auto current = time();
        if (!current)
            return;
        if (*current >= pending_expiry) {
            fail(AdvertisementLeaseFailure::ExpiredPending);
            return;
        }
        pending = false;
        paused = false;
        auto fields = config.fields;
        fields.sequence = pending_sequence;
        fields.issued_at = pending_expiry - config.lifetime;
        fields.expires_at = pending_expiry;
        published_fields = fields;
        callbacks.on_published(pending_sequence);
    } catch (const std::exception &) {
        fail(AdvertisementLeaseFailure::EncodingFailed);
    }
}
std::optional<std::uint64_t> NativeAdvertisementLease::State::time() {
    const auto current = now();
    if (last_now && current < *last_now) {
        fail(AdvertisementLeaseFailure::ClockRegressed);
        return std::nullopt;
    }
    last_now = current;
    return current;
}

bool NativeAdvertisementLease::State::prepare_window(std::uint64_t current) {
    if (!config.windowed)
        return true;
    const auto next = discovery_window_for(current);
    if (window && *window == next)
        return true;
    auto key = derive_discovery_window_key(*capability, next);
    if (std::holds_alternative<CryptoError>(key)) {
        fail(AdvertisementLeaseFailure::EncodingFailed);
        return false;
    }
    auto replacement = std::make_unique<NativeOutbox>(store, std::get<DiscoveryKey>(key), budget,
                                                      config.outbox, executor, handlers());
    outbox->close();
    outbox = std::move(replacement);
    window = next;
    publications = 0;
    return true;
}

bool NativeAdvertisementLease::State::can_publish(std::uint64_t current) {
    if (!prepare_window(current))
        return false;
    if (publications >= config.max_publications) {
        fail(AdvertisementLeaseFailure::PublicationLimit);
        return false;
    }
    if (exhausted) {
        fail(AdvertisementLeaseFailure::SequenceExhausted);
        return false;
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (config.lifetime > maximum - current || config.refresh_interval > maximum - current) {
        fail(AdvertisementLeaseFailure::TimeOverflow);
        return false;
    }
    return true;
}

AdvertisementLeaseTick NativeAdvertisementLease::State::publish(std::uint64_t current) {
    if (!can_publish(current))
        return AdvertisementLeaseTick::Failed;
    auto fields = config.fields;
    fields.issued_at = current;
    fields.expires_at = current + config.lifetime;
    auto wire = encode_advertisement(*identity, fields, *capability,
                                     {current, config.max_lifetime, config.clock_skew});
    if (!std::holds_alternative<Bytes>(wire)) {
        fail(AdvertisementLeaseFailure::EncodingFailed);
        return AdvertisementLeaseTick::Failed;
    }
    pending = true;
    pending_sequence = fields.sequence;
    pending_expiry = fields.expires_at;
    next_refresh = current + config.refresh_interval;
    if (outbox->enqueue(std::move(std::get<Bytes>(wire))) != SignalingQueueResult::Accepted) {
        pending = false;
        fail(AdvertisementLeaseFailure::AdmissionFailed);
        return AdvertisementLeaseTick::Failed;
    }
    ++publications;
    if (fields.sequence == std::numeric_limits<std::uint64_t>::max())
        exhausted = true;
    else
        ++config.fields.sequence;
    return AdvertisementLeaseTick::Submitted;
}

OutboxCallbacks NativeAdvertisementLease::State::handlers() {
    auto weak = weak_from_this();
    return {[weak] {
                if (auto self = weak.lock(); self && self->active && self->healthy) {
                    self->complete();
                }
            },
            [weak](OutboxError error) {
                if (auto self = weak.lock(); self && self->active && self->healthy) {
                    self->paused = true;
                    self->callbacks.on_error(error);
                }
            }};
}

NativeAdvertisementLease::NativeAdvertisementLease(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeAdvertisementLease::~NativeAdvertisementLease() { close(); }

std::variant<std::unique_ptr<NativeAdvertisementLease>, CryptoError>
NativeAdvertisementLease::create(RecordStore store, std::shared_ptr<const Identity> identity,
                                 std::shared_ptr<const Bytes> capability,
                                 AdvertisementLeaseConfig config, MemoryBudget budget,
                                 Executor executor, std::function<std::uint64_t()> now,
                                 AdvertisementLeaseCallbacks callbacks) {
    if (!executor || !now || !identity || !capability || !callbacks.on_published ||
        !callbacks.on_error || config.lifetime == 0 || config.lifetime > config.max_lifetime ||
        config.refresh_interval == 0 || config.refresh_interval >= config.lifetime ||
        config.max_publications == 0 || config.outbox.record_bytes < advertisement_wire_size ||
        !nonzero(identity->public_key()) || !nonzero(config.fields.recipient_public_key) ||
        !nonzero(config.fields.instance_id) || !nonzero(config.fields.signaling_inbox)) {
        throw std::invalid_argument("invalid native advertisement lease configuration");
    }
    auto key = config.windowed ? derive_discovery_window_key(*capability, 0)
                               : derive_discovery_key(*capability);
    if (auto *error = std::get_if<CryptoError>(&key))
        return *error;
    auto state = std::make_shared<State>(std::move(identity), std::move(capability), config,
                                         std::move(now), std::move(callbacks), std::move(store),
                                         std::move(budget), std::move(executor));
    state->outbox =
        std::make_unique<NativeOutbox>(state->store, std::get<DiscoveryKey>(key), state->budget,
                                       config.outbox, state->executor, state->handlers());
    if (config.windowed)
        state->window = 0;
    return std::unique_ptr<NativeAdvertisementLease>(
        new NativeAdvertisementLease(std::move(state)));
}

AdvertisementLeaseTick NativeAdvertisementLease::tick() {
    auto state = state_;
    if (!state->active)
        return AdvertisementLeaseTick::Closed;
    if (!state->healthy)
        return AdvertisementLeaseTick::Failed;
    if (!state->outbox->is_open()) {
        state->fail(AdvertisementLeaseFailure::PublisherUnavailable);
        return AdvertisementLeaseTick::Failed;
    }
    try {
        auto current = state->time();
        if (!current)
            return AdvertisementLeaseTick::Failed;
        if (state->pending && *current >= state->pending_expiry) {
            state->fail(AdvertisementLeaseFailure::ExpiredPending);
            return AdvertisementLeaseTick::Failed;
        }
        if (state->pending)
            return AdvertisementLeaseTick::Pending;
        if (state->last_now && *current < state->next_refresh)
            return AdvertisementLeaseTick::NotDue;
        return state->publish(*current);
    } catch (const std::exception &) {
        state->fail(AdvertisementLeaseFailure::EncodingFailed);
        return AdvertisementLeaseTick::Failed;
    }
}

bool NativeAdvertisementLease::retry() {
    auto state = state_;
    if (!state->active || !state->healthy || !state->pending || !state->paused ||
        !state->outbox->is_open())
        return false;
    try {
        auto current = state->time();
        if (!current)
            return false;
        if (*current >= state->pending_expiry) {
            state->fail(AdvertisementLeaseFailure::ExpiredPending);
            return false;
        }
        if (state->publications >= state->config.max_publications) {
            state->fail(AdvertisementLeaseFailure::PublicationLimit);
            return false;
        }
        state->paused = false;
        if (state->outbox->retry()) {
            ++state->publications;
            return true;
        }
        state->paused = true;
        return false;
    } catch (const std::exception &) {
        state->fail(AdvertisementLeaseFailure::EncodingFailed);
        return false;
    }
}
bool NativeAdvertisementLease::is_open() const {
    return state_->active && state_->healthy && state_->outbox->is_open();
}
std::optional<AdvertisementFields> NativeAdvertisementLease::published_fields() {
    auto state = state_;
    if (!is_open() || !state->published_fields) {
        return std::nullopt;
    }
    try {
        auto current = state->time();
        if (!current || *current >= state->published_fields->expires_at) {
            return std::nullopt;
        }
        return state->published_fields;
    } catch (const std::exception &) {
        state->fail(AdvertisementLeaseFailure::EncodingFailed);
        return std::nullopt;
    }
}
void NativeAdvertisementLease::close() {
    state_->active = false;
    state_->outbox->close();
}

} // namespace p2pdc::detail
