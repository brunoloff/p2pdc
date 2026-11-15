#include "p2pdc/runtime.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace p2pdc {

struct Runtime::State : std::enable_shared_from_this<State> {
    using Clock = std::chrono::steady_clock;
    struct Event {
        std::weak_ptr<State> owner;
        std::uint64_t id;
        Clock::time_point deadline;
        std::function<void()> task;
        int timer = 0;
    };
    std::map<std::uint64_t, std::unique_ptr<Event>> events;
    std::uint64_t next_id = 0;
    bool stopped = false;

    CancelTimer schedule(std::chrono::milliseconds delay, std::function<void()> task);
    void arm(Event &event);
    void fire(std::uint64_t id);
    void cancel(std::uint64_t id);
    void stop();
};

void Runtime::State::arm(Event &event) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::max(event.deadline - Clock::now(), Clock::duration::zero()));
    const auto delay = std::min<std::int64_t>(remaining.count(), std::numeric_limits<int>::max());
    event.timer = emscripten_set_timeout(
        [](void *data) {
            auto *item = static_cast<Event *>(data);
            if (auto owner = item->owner.lock())
                owner->fire(item->id);
        },
        static_cast<double>(delay), &event);
}

CancelTimer Runtime::State::schedule(std::chrono::milliseconds delay, std::function<void()> task) {
    if (!task || delay < std::chrono::milliseconds::zero())
        throw std::invalid_argument("runtime requires a task and nonnegative timer delay");
    if (stopped)
        throw std::runtime_error("runtime is stopped");
    const auto now = Clock::now();
    if (delay > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now) ||
        next_id == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("timer delay or count is too large");
    const auto id = ++next_id;
    auto weak = weak_from_this();
    CancelTimer cancel = [weak, id] {
        if (auto owner = weak.lock())
            owner->cancel(id);
    };
    auto event = std::make_unique<Event>(Event{weak, id, now + delay, std::move(task)});
    auto *item = event.get();
    events.emplace(id, std::move(event));
    arm(*item);
    return cancel;
}

void Runtime::State::fire(std::uint64_t id) {
    auto found = events.find(id);
    if (found == events.end() || stopped)
        return;
    if (Clock::now() < found->second->deadline) {
        arm(*found->second);
        return;
    }
    auto event = std::move(found->second);
    events.erase(found);
    event->task();
}

void Runtime::State::cancel(std::uint64_t id) {
    auto found = events.find(id);
    if (found == events.end())
        return;
    auto event = std::move(found->second);
    events.erase(found);
    emscripten_clear_timeout(event->timer);
    emscripten_async_call([](void *data) { delete static_cast<Event *>(data); }, event.release(), 0);
}

void Runtime::State::stop() {
    stopped = true;
    while (!events.empty())
        cancel(events.begin()->first);
}

Runtime::Runtime() : state_(std::make_shared<State>()) {}
Runtime::~Runtime() { state_->stop(); }

Executor Runtime::executor() const {
    std::weak_ptr<State> weak = state_;
    return [weak](std::function<void()> task) {
        auto state = weak.lock();
        if (!state)
            throw std::runtime_error("runtime is destroyed");
        (void)state->schedule(std::chrono::milliseconds::zero(), std::move(task));
    };
}

Scheduler Runtime::scheduler() const {
    std::weak_ptr<State> weak = state_;
    return [weak](std::chrono::milliseconds delay, std::function<void()> task) {
        auto state = weak.lock();
        if (!state)
            throw std::runtime_error("runtime is destroyed");
        return state->schedule(delay, std::move(task));
    };
}

} // namespace p2pdc
