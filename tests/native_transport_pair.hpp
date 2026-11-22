#pragma once

#include "../src/transport/native_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace p2pdc::detail::testing {
class Queue {
  public:
    Executor executor() {
        return [this](std::function<void()> action) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(action));
        };
    }
    void drain() {
        for (int count = 0; count < 10000; ++count) {
            std::function<void()> action;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty()) {
                    return;
                }
                action = std::move(tasks_.front());
                tasks_.pop_front();
            }
            action();
        }
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        drain();
        return done();
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

struct Pair {
    Queue queue;
    std::unique_ptr<NativeTransport> a;
    std::unique_ptr<NativeTransport> b;
    std::vector<Bytes> received;
    std::vector<std::string> errors;
    std::vector<ConnectError> error_categories;
    std::vector<NativeTransportState> states;
    int notifications = 0;
    bool hold_a = false; // a takes no messages while set

    NativeTransportCallbacks callbacks(bool first) {
        NativeTransportCallbacks result;
        result.on_description = [this, first](rtc::Description description) {
            ++notifications;
            auto &other = first ? b : a;
            if (other && other->state() != NativeTransportState::Closed) {
                other->set_remote_description(std::move(description));
            }
        };
        result.on_candidate = [this, first](rtc::Candidate candidate) {
            ++notifications;
            auto &other = first ? b : a;
            if (other && other->state() != NativeTransportState::Closed) {
                other->add_remote_candidate(std::move(candidate));
            }
        };
        result.on_state = [this, first](NativeTransportState state) {
            ++notifications;
            if (!first) {
                states.push_back(state);
            }
        };
        result.on_error = [this](ConnectError category, std::string error) {
            error_categories.push_back(category);
            errors.push_back(std::move(error));
        };
        result.on_binary = [this, first](Bytes bytes) {
            if (first) {
                received.push_back(std::move(bytes));
            } else {
                EXPECT_EQ(b->send(bytes), NativeSendResult::Accepted);
            }
        };
        if (first) {
            result.can_receive = [this] { return !hold_a; };
        }
        return result;
    }
    void create(rtc::Configuration config = {},
                NativeRelayPolicy second = NativeRelayPolicy::Automatic,
                NativeReceiveLimits limits = {}) {
        a = std::make_unique<NativeTransport>(config, NativeRelayPolicy::Automatic,
                                              queue.executor(), callbacks(true), limits);
        b = std::make_unique<NativeTransport>(config, second, queue.executor(), callbacks(false),
                                              std::move(limits));
    }
    bool connect() {
        a->start_offer();
        return queue.until([this] {
            return a->state() == NativeTransportState::Ready &&
                   b->state() == NativeTransportState::Ready;
        });
    }
};
} // namespace p2pdc::detail::testing
