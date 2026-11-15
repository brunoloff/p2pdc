#include "timer_producer.hpp"
#include "submission_gate.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
struct Ticket {
    std::atomic_bool active{true};
    std::atomic_bool fired{false};
};
} // namespace

struct TimerProducer::State : std::enable_shared_from_this<State> {
    SubmissionGate gate;
    Scheduler scheduler;
    std::shared_ptr<Ticket> ticket;
    CancelTimer cancel;
    std::atomic_bool active{true};
    std::atomic_bool healthy{true};

    State(Executor executor, Scheduler schedule)
        : gate(std::move(executor)), scheduler(std::move(schedule)) {}
    void fire(const std::shared_ptr<Ticket> &current,
              const std::shared_ptr<std::function<void()>> &callback) noexcept;
};

void TimerProducer::State::fire(const std::shared_ptr<Ticket> &current,
                                const std::shared_ptr<std::function<void()>> &callback) noexcept {
    if (!active || !current->active || current->fired.exchange(true))
        return;
    try {
        (void)gate.post([weak = weak_from_this(), current, callback] {
            if (auto self = weak.lock(); self && self->active && current->active)
                (*callback)();
        });
    } catch (const std::exception &error) {
        current->active = false;
        healthy = false;
        std::cerr << "TimerProducer: cannot submit timer callback: " << error.what() << '\n';
    }
}

TimerProducer::TimerProducer(Executor executor, Scheduler scheduler)
    : state_(std::make_shared<State>(std::move(executor), std::move(scheduler))) {
    if (!state_->scheduler)
        throw std::invalid_argument("timer producer requires a scheduler");
}
TimerProducer::~TimerProducer() { close(); }

void TimerProducer::arm(std::chrono::milliseconds delay, std::function<void()> callback) {
    if (delay.count() < 0 || !callback)
        throw std::invalid_argument("timer requires a nonnegative delay and callback");
    if (!state_->active || !state_->healthy)
        throw std::logic_error("timer producer is closed or unhealthy");
    cancel();
    auto ticket = std::make_shared<Ticket>();
    auto shared_callback = std::make_shared<std::function<void()>>(std::move(callback));
    auto weak = std::weak_ptr<State>(state_);
    try {
        auto cancellation = state_->scheduler(delay, [weak, ticket, shared_callback] {
            if (auto state = weak.lock())
                state->fire(ticket, shared_callback);
        });
        if (!cancellation)
            throw std::invalid_argument("scheduler returned an empty cancellation function");
        state_->ticket = std::move(ticket);
        state_->cancel = std::move(cancellation);
    } catch (...) {
        ticket->active = false;
        throw;
    }
}

void TimerProducer::cancel() {
    if (state_->ticket)
        state_->ticket->active = false;
    state_->ticket.reset();
    auto cancel = std::move(state_->cancel);
    if (cancel)
        cancel();
}

void TimerProducer::close() {
    if (state_->active.exchange(false)) {
        state_->gate.close();
        cancel();
    }
}

bool TimerProducer::healthy() const noexcept { return state_->active && state_->healthy; }
} // namespace p2pdc::detail
