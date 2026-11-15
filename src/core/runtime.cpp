#include "p2pdc/runtime.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace p2pdc {

struct Runtime::State : std::enable_shared_from_this<State> {
    using Clock = std::chrono::steady_clock;
    struct Timer {
        std::shared_ptr<std::atomic_bool> canceled;
        std::function<void()> task;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    std::multimap<Clock::time_point, Timer> timers;
    std::atomic_bool purge_requested{false};
    bool stopping = false;
    bool prefer_task = false;
    std::thread worker;

    void run();
    void post(std::function<void()> task);
    CancelTimer schedule(std::chrono::milliseconds delay, std::function<void()> task);
    void purge_canceled(std::unique_lock<std::mutex> &lock);
    void take_timer(std::function<void()> &task, std::function<void()> &discarded);
};

void Runtime::State::post(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("runtime requires a task");
    }
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            throw std::runtime_error("runtime is stopped");
        }
        tasks.push_back(std::move(task));
    }
    wake.notify_one();
}

CancelTimer Runtime::State::schedule(std::chrono::milliseconds delay, std::function<void()> task) {
    if (!task || delay < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("runtime requires a task and nonnegative timer delay");
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto weak = weak_from_this();
    CancelTimer cancel = [canceled, weak] {
        if (!canceled->exchange(true)) {
            if (auto state = weak.lock()) {
                state->purge_requested.store(true);
                state->wake.notify_one();
            }
        }
    };
    auto now = Clock::now();
    if (delay >
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now)) {
        throw std::invalid_argument("timer delay is too large");
    }
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            throw std::runtime_error("runtime is stopped");
        }
        timers.emplace(now + delay, Timer{std::move(canceled), std::move(task)});
    }
    wake.notify_one();
    return cancel;
}

void Runtime::State::purge_canceled(std::unique_lock<std::mutex> &lock) {
    if (!purge_requested.exchange(false)) {
        return;
    }
    decltype(timers) removed;
    for (auto it = timers.begin(); it != timers.end();) {
        if (it->second.canceled->load()) {
            removed.insert(timers.extract(it++));
        } else {
            ++it;
        }
    }
    lock.unlock();
    removed.clear();
    lock.lock();
}

void Runtime::State::take_timer(std::function<void()> &task, std::function<void()> &discarded) {
    auto timer = std::move(timers.begin()->second);
    timers.erase(timers.begin());
    prefer_task = true;
    if (!timer.canceled->load()) {
        task = std::move(timer.task);
    } else {
        discarded = std::move(timer.task);
    }
}

void Runtime::State::run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
        purge_canceled(lock);
        if (stopping) {
            break;
        }
        std::function<void()> task;
        std::function<void()> discarded;
        const bool timer_ready = !timers.empty() && timers.begin()->first <= Clock::now();
        if (timer_ready && (!prefer_task || tasks.empty())) {
            take_timer(task, discarded);
        } else if (!tasks.empty()) {
            task = std::move(tasks.front());
            tasks.pop_front();
            prefer_task = false;
        } else if (timers.empty()) {
            wake.wait(lock);
        } else {
            // Cancellation does not acquire this mutex, so periodically check
            // the atomic flag even if a notification races with entering wait.
            wake.wait_until(
                lock, std::min(timers.begin()->first, Clock::now() + std::chrono::seconds(1)));
        }
        if (task || discarded) {
            lock.unlock();
            if (task) {
                task();
            }
            discarded = {};
            task = {};
            lock.lock();
        }
    }
    auto abandoned_tasks = std::move(tasks);
    auto abandoned_timers = std::move(timers);
    lock.unlock();
}

Runtime::Runtime() : state_(std::make_shared<State>()) {
    state_->worker = std::thread([state = state_] { state->run(); });
}

Runtime::~Runtime() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_one();
    if (state_->worker.get_id() == std::this_thread::get_id()) {
        state_->worker.detach();
    } else {
        state_->worker.join();
    }
}

Executor Runtime::executor() const {
    std::weak_ptr<State> weak = state_;
    return [weak](std::function<void()> task) {
        auto state = weak.lock();
        if (!state) {
            throw std::runtime_error("runtime is destroyed");
        }
        state->post(std::move(task));
    };
}

Scheduler Runtime::scheduler() const {
    std::weak_ptr<State> weak = state_;
    return [weak](std::chrono::milliseconds delay, std::function<void()> task) {
        auto state = weak.lock();
        if (!state) {
            throw std::runtime_error("runtime is destroyed");
        }
        return state->schedule(delay, std::move(task));
    };
}

} // namespace p2pdc
