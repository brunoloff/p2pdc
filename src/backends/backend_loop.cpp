#include "backend_loop.hpp"

#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace p2pdc::detail {

// Shared with the worker, which may outlive the loop object when stopped from its own thread.
struct BackendLoop::State {
    using Clock = std::chrono::steady_clock;
    struct Due {
        Clock::time_point at;
        TimerId order;
        bool operator<(const Due &other) const {
            return at != other.at ? at < other.at : order < other.order;
        }
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::map<Due, std::function<void()>> tasks;
    std::map<TimerId, Due> timers;
    TimerId next = 0;
    bool stopped = false;
    std::thread thread;
    std::thread::id thread_id;

    static void run(const std::shared_ptr<State> &state);
};

BackendLoop::BackendLoop() : state_(std::make_shared<State>()) {}
BackendLoop::~BackendLoop() { stop(); }

bool BackendLoop::post(std::function<void()> task) {
    return post_after(std::chrono::milliseconds(0), std::move(task)) != 0;
}

BackendLoop::TimerId BackendLoop::post_after(std::chrono::milliseconds delay,
                                             std::function<void()> task) {
    if (!task)
        throw std::invalid_argument("backend loop task is empty");
    std::lock_guard lock(state_->mutex);
    if (state_->stopped)
        return 0;
    const State::Due due{State::Clock::now() + delay, ++state_->next};
    state_->tasks.emplace(due, std::move(task));
    state_->timers.emplace(due.order, due);
    if (!state_->thread.joinable()) {
        state_->thread = std::thread([state = state_] { State::run(state); });
        state_->thread_id = state_->thread.get_id();
    }
    state_->wake.notify_one();
    return due.order;
}

void BackendLoop::cancel(TimerId timer) {
    std::function<void()> dropped; // destroyed outside the lock
    std::lock_guard lock(state_->mutex);
    const auto found = state_->timers.find(timer);
    if (found == state_->timers.end())
        return;
    const auto task = state_->tasks.find(found->second);
    dropped = std::move(task->second);
    state_->tasks.erase(task);
    state_->timers.erase(found);
}

bool BackendLoop::on_loop_thread() const {
    std::lock_guard lock(state_->mutex);
    // The id stays set after stop(), while a detached worker finishes its task.
    return std::this_thread::get_id() == state_->thread_id;
}

void BackendLoop::stop() {
    std::thread worker;
    // Dropped tasks are destroyed outside the lock: their captures may post again.
    std::map<State::Due, std::function<void()>> dropped;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        dropped.swap(state_->tasks);
        state_->timers.clear();
        state_->wake.notify_one();
        if (!state_->thread.joinable())
            return;
        if (std::this_thread::get_id() == state_->thread_id) {
            state_->thread.detach();
            return;
        }
        worker = std::move(state_->thread);
    }
    worker.join();
}

void BackendLoop::State::run(const std::shared_ptr<State> &state) {
    std::unique_lock lock(state->mutex);
    while (!state->stopped) {
        if (state->tasks.empty()) {
            state->wake.wait(lock);
            continue;
        }
        const auto first = state->tasks.begin();
        if (first->first.at > Clock::now()) {
            state->wake.wait_until(lock, first->first.at);
            continue;
        }
        auto task = std::move(first->second);
        state->timers.erase(first->first.order);
        state->tasks.erase(first);
        lock.unlock();
        try {
            task();
        } catch (const std::exception &error) {
            std::cerr << "p2pdc: discovery backend task failed: " << error.what() << '\n';
        }
        task = nullptr; // captures are released outside the lock
        lock.lock();
    }
}

} // namespace p2pdc::detail
