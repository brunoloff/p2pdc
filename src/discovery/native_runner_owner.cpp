#include "native_runner_owner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace p2pdc::detail {
namespace {
std::exception_ptr join_until_stopped(dht::DhtRunner &runner) {
    std::exception_ptr first_failure;
    for (;;) {
        try {
            runner.join();
            return first_failure;
        } catch (...) {
            if (!first_failure) {
                first_failure = std::current_exception();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
} // namespace

struct NativeRunnerOwner::State {
    explicit State(Executor executor) : submissions(std::move(executor)) {}
    SubmissionGate submissions;
    std::shared_ptr<dht::DhtRunner> runner = std::make_shared<dht::DhtRunner>();
    std::atomic_bool abandoned{false};
    std::atomic_bool delivery_failed{false};
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    Stopped stopped;
};

NativeRunnerOwner::NativeRunnerOwner(std::shared_ptr<State> state) : state_(std::move(state)) {}

std::unique_ptr<NativeRunnerOwner> NativeRunnerOwner::create(Executor executor, RunnerStart start,
                                                             Started started, RunnerQuota quota) {
    if (!executor || !start || !started) {
        throw std::invalid_argument("native runner requires executor, start, and callback");
    }
    auto permit = quota.try_acquire();
    if (!permit) {
        return nullptr;
    }
    auto state = std::make_shared<State>(std::move(executor));
    auto owner = std::unique_ptr<NativeRunnerOwner>(new NativeRunnerOwner(state));
    struct WorkerContext {
        RunnerPermit permit;
        std::shared_ptr<State> state;
        RunnerStart start;
        Started started;
    };
    std::thread worker([context = WorkerContext{std::move(*permit), state, std::move(start),
                                                std::move(started)}]() mutable {
        try {
            work(context.state, std::move(context.start), std::move(context.started));
        } catch (...) {
            context.state->delivery_failed.store(true);
            (void)join_until_stopped(*context.state->runner);
        }
    });
    worker.detach();
    return owner;
}

NativeRunnerOwner::~NativeRunnerOwner() {
    state_->abandoned.store(true);
    state_->submissions.close();
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_one();
}

bool NativeRunnerOwner::shutdown(Stopped stopped) {
    if (!stopped) {
        throw std::invalid_argument("native runner shutdown requires a callback");
    }
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) {
            return false;
        }
        state_->stopping = true;
        state_->stopped = std::move(stopped);
    }
    state_->wake.notify_one();
    return true;
}

bool NativeRunnerOwner::delivery_failed() const noexcept { return state_->delivery_failed.load(); }

void NativeRunnerOwner::work(std::shared_ptr<State> state, RunnerStart start, Started started) {
    auto runner = state->runner;
    RunnerStartResult result = runner;
    try {
        start(*runner);
    } catch (...) {
        result = std::current_exception();
        (void)join_until_stopped(*runner);
    }
    publish_start(state, std::move(started), std::move(result));
    {
        std::unique_lock lock(state->mutex);
        state->wake.wait(lock, [&] { return state->stopping; });
    }
    auto error = join_until_stopped(*runner);
    publish_stop(state, error);
}

void NativeRunnerOwner::publish_start(const std::shared_ptr<State> &state, Started started,
                                      RunnerStartResult result) {
    try {
        (void)state->submissions.post(
            [state, started = std::move(started), result = std::move(result)]() mutable {
                if (state->abandoned.load()) {
                    return;
                }
                bool canceled;
                {
                    std::lock_guard lock(state->mutex);
                    canceled = state->stopping;
                }
                if (canceled) {
                    result = RunnerCanceled{};
                }
                started(std::move(result));
            });
    } catch (...) {
        state->delivery_failed.store(true);
    }
}

void NativeRunnerOwner::publish_stop(const std::shared_ptr<State> &state,
                                     std::exception_ptr error) {
    Stopped stopped;
    {
        std::lock_guard lock(state->mutex);
        stopped = std::move(state->stopped);
    }
    if (stopped) {
        try {
            (void)state->submissions.post([state, stopped = std::move(stopped), error]() mutable {
                if (!state->abandoned.load()) {
                    stopped(error);
                }
            });
        } catch (...) {
            state->delivery_failed.store(true);
        }
    }
}
} // namespace p2pdc::detail
