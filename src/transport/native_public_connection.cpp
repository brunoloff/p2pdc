#include "native_public_connection.hpp"
#include "native_public_connection_testing.hpp"

#include "../core/deferred_events.hpp"
#include "../core/timer_producer.hpp"
#include "connection_status.hpp"
#include "datagram_delivery.hpp"
#include "public_attempt.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
class PublicConnection final : public ConnectionBackend,
                               public std::enable_shared_from_this<PublicConnection> {
  public:
    PublicConnection(Executor executor, CallbackGate gate, Scheduler scheduler,
                     ConnectOptions options, ConnectionHandlers handlers,
                     std::function<bool()> revalidate);
    void initialize();
    [[nodiscard]] std::optional<ConnectError>
    start(const ConnectionAdmission &admission, IceConfig ice, const IncomingOffer *offer) override;
    void close() override;
    void supersede() override { fail(ConnectError::Superseded); }
    [[nodiscard]] ConnectionState state() const override;
    [[nodiscard]] RouteInfo route() const override {
        return state() == ConnectionState::Failed ? invalidated_route(route_) : route_;
    }
    [[nodiscard]] std::optional<std::uint32_t> max_message_size() const override;
    [[nodiscard]] SendStatus send(ByteView message) override;
    [[nodiscard]] bool datagrams_available() const override;
    [[nodiscard]] SendStatus send_datagram(ByteView message) override;
    [[nodiscard]] NativeAttempt *attempt_for_testing() const {
        return attempt_ ? attempt_->native_for_testing() : nullptr;
    }
    [[nodiscard]] bool status_queued_for_testing() const { return signaling_unavailable_; }

  private:
    [[nodiscard]] ConnectionAttemptCallbacks callbacks();
    void tick();
    void update_state();
    void notify(std::function<void(ConnectionHandlers &)> call) noexcept;
    void notify_state(ConnectionState value) noexcept;
    void ready() noexcept;
    void message(Bytes value) noexcept;
    void datagram(Bytes value) noexcept;
    void writable() noexcept;
    void changed_route(RouteInfo value) noexcept;
    void enqueue(std::size_t bytes, std::function<void()> callback) noexcept;
    void invoke(const std::function<void()> &callback);
    void fail(ConnectError error) noexcept;
    void cleanup();
    Executor executor_;
    CallbackGate gate_;
    Scheduler scheduler_;
    ConnectOptions options_;
    ConnectionHandlers handlers_;
    std::function<bool()> revalidate_;
    std::unique_ptr<TimerProducer> timer_;
    std::unique_ptr<DeferredEvents> events_;
    std::unique_ptr<DatagramDelivery> datagrams_;
    std::unique_ptr<PublicAttempt> attempt_;
    MemoryBudget budget_;
    std::chrono::steady_clock::time_point deadline_;
    RouteInfo route_;
    ConnectionState state_ = ConnectionState::Connecting;
    std::optional<ConnectError> failure_;
    bool active_ = true;
    bool failed_ = false;
    bool started_ = false;
    bool authenticated_ = false;
    bool signaling_unavailable_ = false;
};

PublicConnection::PublicConnection(Executor executor, CallbackGate gate, Scheduler scheduler,
                                   ConnectOptions options, ConnectionHandlers handlers,
                                   std::function<bool()> revalidate)
    : executor_(std::move(executor)), gate_(gate), scheduler_(std::move(scheduler)),
      options_(options), handlers_(std::move(handlers)), revalidate_(std::move(revalidate)),
      budget_(std::max<std::size_t>(4 * 1024 * 1024, 2 * options.max_message_bytes + 1024 * 1024)) {
}
void PublicConnection::initialize() {
    auto weak = weak_from_this();
    auto executor = executor_;
    Executor backend = [weak, executor](auto task) {
        if (auto self = weak.lock()) {
            executor([self, task = std::move(task)]() mutable { task(); });
        }
    };
    auto gate = gate_;
    Executor delivery = [weak, gate](auto task) {
        auto self = weak.lock();
        if (!self || !gate.post([self, task = std::move(task)]() mutable { task(); })) {
            throw std::logic_error("node callback gate closed during connection");
        }
    };
    timer_ = std::make_unique<TimerProducer>(backend, std::move(scheduler_));
    datagrams_ = std::make_unique<DatagramDelivery>(delivery);
    events_ = std::make_unique<DeferredEvents>(
        std::move(delivery), MemoryBudget(options_.max_pending_bytes), options_.max_pending_events,
        options_.max_pending_bytes);
    executor_ = std::move(backend);
}
std::optional<ConnectError> PublicConnection::start(const ConnectionAdmission &admission,
                                                    IceConfig ice, const IncomingOffer *offer) {
    if (!active_ || attempt_) {
        throw std::logic_error("connection owner already started or closed");
    }
    auto created =
        make_public_attempt(admission, offer, std::move(ice), budget_, options_.max_message_bytes,
                            options_.datagrams, executor_, callbacks());
    if (auto error = std::get_if<StoreError>(&created)) {
        return connection_store_error(*error);
    }
    attempt_ = std::move(std::get<std::unique_ptr<PublicAttempt>>(created));
    deadline_ = std::chrono::steady_clock::now() + options_.timeout;
    tick();
    return failure_;
}
void PublicConnection::cleanup() {
    if (timer_) {
        timer_->close();
    }
    if (attempt_) {
        attempt_->close();
        attempt_.reset();
    }
}
void PublicConnection::close() {
    if (!active_) {
        return;
    }
    active_ = false;
    state_ = ConnectionState::Closed;
    route_ = invalidated_route(route_);
    if (events_) {
        events_->close();
        datagrams_->close();
    }
    cleanup();
}
ConnectionState PublicConnection::state() const {
    return active_ && timer_ && !timer_->healthy() ? ConnectionState::Failed : state_;
}
std::optional<std::uint32_t> PublicConnection::max_message_size() const {
    if (!active_ || state() == ConnectionState::Failed || !attempt_)
        return std::nullopt;
    return attempt_->max_message_size();
}
SendStatus PublicConnection::send(ByteView message) {
    if (!active_) {
        return SendStatus::Closed;
    }
    if (failed_ || !attempt_ || !timer_->healthy()) {
        return SendStatus::Failed;
    }
    return public_send_status(attempt_->send(message));
}
bool PublicConnection::datagrams_available() const {
    return active_ && !failed_ && attempt_ && attempt_->datagrams_available();
}
SendStatus PublicConnection::send_datagram(ByteView message) {
    if (!active_) {
        return SendStatus::Closed;
    }
    if (failed_ || !attempt_ || !timer_->healthy()) {
        return SendStatus::Failed;
    }
    return public_send_status(attempt_->send_datagram(message));
}
void PublicConnection::invoke(const std::function<void()> &callback) {
    try {
        callback();
    } catch (...) {
        fail(ConnectError::Callback);
        throw;
    }
}
void PublicConnection::enqueue(std::size_t bytes, std::function<void()> callback) noexcept {
    if (!active_ || failed_) {
        return;
    }
    try {
        if (events_->post(bytes, std::move(callback)) != DeferredEventResult::Accepted) {
            fail(ConnectError::ResourceLimit);
        }
    } catch (...) {
        fail(ConnectError::ResourceLimit);
    }
}
void PublicConnection::notify(std::function<void(ConnectionHandlers &)> call) noexcept {
    try {
        enqueue(0, [weak = weak_from_this(), call = std::move(call)] {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                self->invoke([&] { call(self->handlers_); });
            }
        });
    } catch (...) {
        fail(ConnectError::ResourceLimit);
    }
}
void PublicConnection::notify_state(ConnectionState value) noexcept {
    notify([value](ConnectionHandlers &handlers) { handlers.on_state(value); });
}
void PublicConnection::ready() noexcept {
    if (!active_ || failed_) {
        return;
    }
    authenticated_ = true;
    state_ = ConnectionState::Ready;
    notify_state(ConnectionState::Ready);
}
void PublicConnection::message(Bytes value) noexcept {
    try {
        const auto size = value.size();
        enqueue(size, [weak = weak_from_this(), value = std::move(value)]() mutable {
            if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
                self->invoke([&] { self->handlers_.on_message(std::move(value)); });
            }
        });
    } catch (...) {
        fail(ConnectError::ResourceLimit);
    }
}
void PublicConnection::datagram(Bytes value) noexcept {
    if (!active_ || failed_ || !handlers_.on_datagram) {
        return;
    }
    datagrams_->post(std::move(value), [weak = weak_from_this()](Bytes bytes) {
        if (auto self = weak.lock(); self && self->active_ && !self->failed_) {
            self->invoke([&] { self->handlers_.on_datagram(std::move(bytes)); });
        }
    });
}
void PublicConnection::writable() noexcept {
    notify([](ConnectionHandlers &handlers) { handlers.on_writable(); });
}
void PublicConnection::changed_route(RouteInfo value) noexcept {
    if (!active_ || failed_) {
        return;
    }
    route_ = value;
    notify([value](ConnectionHandlers &handlers) { handlers.on_route(value); });
}
void PublicConnection::fail(ConnectError error) noexcept {
    if (!active_ || failed_) {
        return;
    }
    failed_ = true;
    failure_ = error;
    state_ = ConnectionState::Failed;
    route_ = invalidated_route(route_);
    try {
        (void)gate_.post([self = shared_from_this(), error] {
            if (self->active_) {
                self->events_->close();
                self->datagrams_->close();
                self->cleanup();
                if (self->active_) {
                    self->handlers_.on_state(ConnectionState::Failed);
                    if (self->active_) {
                        self->handlers_.on_error(error);
                    }
                }
            }
        });
    } catch (...) {
        std::cerr << "p2pdc: connection failure delivery failed; close the connection\n";
    }
}
void PublicConnection::update_state() {
    const auto current = attempt_->state();
    if (current == ConnectionAttemptState::Failed || current == ConnectionAttemptState::Closed) {
        fail(attempt_->failure_reason());
    } else if (current == ConnectionAttemptState::Suspended && state_ == ConnectionState::Ready) {
        state_ = ConnectionState::Suspended;
        notify_state(ConnectionState::Suspended);
    } else if (current == ConnectionAttemptState::Ready && state_ == ConnectionState::Suspended) {
        ready();
    }
}
void PublicConnection::tick() {
    if (!active_ || failed_ || !attempt_) {
        return;
    }
    try {
        if (!authenticated_ && std::chrono::steady_clock::now() >= deadline_) {
            fail(ConnectError::Timeout);
            return;
        }
        if (!started_) {
            if (!revalidate_()) {
                fail(ConnectError::InvalidPeer);
                return;
            }
            const auto result = attempt_->poll_start();
            started_ = result == ConnectionAttemptStart::Started ||
                       result == ConnectionAttemptStart::AlreadyStarted;
            if (started_) {
                revalidate_ = {};
            }
            if (result == ConnectionAttemptStart::Failed ||
                result == ConnectionAttemptStart::Closed) {
                fail(attempt_->failure_reason());
                return;
            }
        } else {
            attempt_->poll_health();
            (void)attempt_->poll_route();
            update_state();
            if (attempt_->signaling_unavailable() && !signaling_unavailable_) {
                signaling_unavailable_ = true;
                enqueue(0, [weak = weak_from_this()] {
                    if (auto self = weak.lock();
                        self && self->active_ && !self->failed_ && self->handlers_.on_status)
                        self->invoke([&] {
                            self->handlers_.on_status(ConnectionStatus::SignalingUnavailable);
                        });
                });
            }
        }
    } catch (const std::exception &) {
        fail(ConnectError::Backend);
        return;
    }
    try {
        if (!failed_) {
            timer_->arm(options_.poll_interval, [weak = weak_from_this()] {
                if (auto self = weak.lock()) {
                    self->tick();
                }
            });
        }
    } catch (const std::exception &) {
        fail(ConnectError::Scheduler);
    }
}
ConnectionAttemptCallbacks PublicConnection::callbacks() {
    auto weak = weak_from_this();
    return {[weak] {
                if (auto self = weak.lock())
                    self->ready();
            },
            [weak](Bytes value) {
                if (auto self = weak.lock())
                    self->message(std::move(value));
            },
            [weak] {
                if (auto self = weak.lock())
                    self->writable();
            },
            [weak](RouteInfo value) {
                if (auto self = weak.lock())
                    self->changed_route(value);
            },
            [weak] {
                if (auto self = weak.lock())
                    self->fail(self->attempt_ ? self->attempt_->failure_reason()
                                              : ConnectError::Backend);
            },
            [weak](Bytes value) {
                if (auto self = weak.lock())
                    self->datagram(std::move(value));
            }};
}
} // namespace
NativeAttempt *native_attempt_for_testing(ConnectionBackend &connection) {
    auto *owner = dynamic_cast<PublicConnection *>(&connection);
    return owner ? owner->attempt_for_testing() : nullptr;
}
bool native_status_queued_for_testing(ConnectionBackend &connection) {
    auto *owner = dynamic_cast<PublicConnection *>(&connection);
    return owner && owner->status_queued_for_testing();
}
std::shared_ptr<ConnectionBackend> make_public_connection(Executor executor, CallbackGate gate,
                                                          Scheduler scheduler,
                                                          ConnectOptions options,
                                                          ConnectionHandlers handlers,
                                                          std::function<bool()> revalidate) {
    validate_public_connection_settings(executor, scheduler, options, handlers);
    if (!revalidate)
        throw std::invalid_argument("connection requires revalidation");
    auto state =
        std::make_shared<PublicConnection>(std::move(executor), gate, std::move(scheduler), options,
                                           std::move(handlers), std::move(revalidate));
    state->initialize();
    return state;
}
} // namespace p2pdc::detail
