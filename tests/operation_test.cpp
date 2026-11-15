#include "p2pdc/operation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

using namespace p2pdc;

TEST(Operation, CompletesAsynchronouslyAndOnlyOnce) {
    std::vector<std::function<void()>> tasks;
    std::optional<OperationResult<int>> result;
    CompletionSource<int> source([&](auto task) { tasks.push_back(std::move(task)); },
                                 [&](auto value) { result = std::move(value); });
    auto operation = source.operation();
    EXPECT_TRUE(source.complete(42));
    EXPECT_FALSE(operation.cancel());
    EXPECT_FALSE(source.complete(7));
    EXPECT_FALSE(result);
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<int>(*result), 42);
}

TEST(Operation, AbandonSuppressesQueuedCompletion) {
    std::vector<std::function<void()>> tasks;
    bool called = false;
    CompletionSource<int> source([&](auto task) { tasks.push_back(std::move(task)); },
                                 [&](auto) { called = true; });
    EXPECT_TRUE(source.complete(42));
    source.abandon();
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_FALSE(called);
    EXPECT_FALSE(source.operation().cancel());
    EXPECT_FALSE(source.complete(7));
}

TEST(Operation, AbandonBeforeCompletionRejectsCompletionAndCancellation) {
    std::vector<std::function<void()>> tasks;
    CompletionSource<int> source([&](auto task) { tasks.push_back(std::move(task)); },
                                 [](auto) { FAIL() << "abandoned callback ran"; });
    source.abandon();
    source.abandon();
    EXPECT_FALSE(source.complete(42));
    EXPECT_FALSE(source.operation().cancel());
    EXPECT_TRUE(tasks.empty());
}

TEST(Operation, CompletionCanDestroyLastSourceDuringSubmission) {
    std::optional<CompletionSource<int>> source;
    source.emplace([](auto task) { std::thread(std::move(task)).join(); },
                   [&](auto) { source.reset(); });
    EXPECT_TRUE(source->complete(1));
    EXPECT_FALSE(source);
}

TEST(Operation, CancellationCanDestroyLastHandleDuringSubmission) {
    std::optional<Operation<int>> operation;
    {
        CompletionSource<int> source([](auto task) { std::thread(std::move(task)).join(); },
                                     [&](auto) { operation.reset(); });
        operation = source.operation();
    }
    EXPECT_TRUE(operation->cancel());
    EXPECT_FALSE(operation);
}

TEST(Operation, CancellationWinsAndConfigurationIsValidated) {
    EXPECT_THROW((CompletionSource<int>{{}, [](auto) {}}), std::invalid_argument);
    EXPECT_THROW((CompletionSource<int>{[](auto) {}, {}}), std::invalid_argument);
    std::vector<std::function<void()>> tasks;
    std::optional<OperationResult<int>> result;
    CompletionSource<int> source([&](auto task) { tasks.push_back(std::move(task)); },
                                 [&](auto value) { result = std::move(value); });
    EXPECT_TRUE(source.operation().cancel());
    EXPECT_FALSE(source.complete(9));
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<OperationError>(*result), OperationError::Canceled);
}

TEST(Operation, FailedSubmissionRestoresCallbackForCompletionRetry) {
    std::vector<std::function<void()>> tasks;
    bool fail = true;
    std::optional<OperationResult<int>> result;
    CompletionSource<int> source(
        [&](auto task) {
            if (fail) {
                fail = false;
                throw std::bad_alloc();
            }
            tasks.push_back(std::move(task));
        },
        [&](auto value) { result = std::move(value); });
    EXPECT_THROW(static_cast<void>(source.complete(1)), std::bad_alloc);
    EXPECT_TRUE(source.complete(2));
    EXPECT_FALSE(source.operation().cancel());
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<int>(*result), 2);
}

TEST(Operation, FailedSubmissionCanBeFollowedByCancellation) {
    std::vector<std::function<void()>> tasks;
    bool fail = true;
    std::optional<OperationResult<int>> result;
    CompletionSource<int> source(
        [&](auto task) {
            if (fail) {
                fail = false;
                throw std::bad_alloc();
            }
            tasks.push_back(std::move(task));
        },
        [&](auto value) { result = std::move(value); });
    EXPECT_THROW(static_cast<void>(source.complete(1)), std::bad_alloc);
    EXPECT_TRUE(source.operation().cancel());
    EXPECT_FALSE(source.complete(2));
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<OperationError>(*result), OperationError::Canceled);
}

TEST(Operation, FailedSubmissionConsumesMoveOnlyResultAndAllowsRetry) {
    struct Tracked {
        int *destroyed;
        ~Tracked() { ++*destroyed; }
    };
    int destroyed = 0;
    bool fail = true;
    std::vector<std::function<void()>> tasks;
    CompletionSource<std::unique_ptr<Tracked>> source(
        [&](auto task) {
            if (fail) {
                fail = false;
                throw std::bad_alloc();
            }
            tasks.push_back(std::move(task));
        },
        [](auto) {});
    EXPECT_THROW(static_cast<void>(source.complete(std::make_unique<Tracked>(&destroyed))),
                 std::bad_alloc);
    EXPECT_EQ(destroyed, 1);
    EXPECT_TRUE(source.complete(std::make_unique<Tracked>(&destroyed)));
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_EQ(destroyed, 2);
}

TEST(Operation, FailedSubmissionResultDestructorCanReenterCancellation) {
    struct Reenter {
        std::function<void()> on_destroy;
        ~Reenter() { on_destroy(); }
    };
    std::vector<std::function<void()>> tasks;
    bool fail = true;
    bool canceled = false;
    using Value = std::unique_ptr<Reenter>;
    std::optional<Operation<Value>> operation;
    CompletionSource<Value> source(
        [&](auto task) {
            if (fail) {
                fail = false;
                throw std::bad_alloc();
            }
            tasks.push_back(std::move(task));
        },
        [&](auto result) { canceled = std::holds_alternative<OperationError>(result); });
    operation = source.operation();
    auto result = std::make_unique<Reenter>([&] { EXPECT_TRUE(operation->cancel()); });
    EXPECT_THROW(static_cast<void>(source.complete(std::move(result))), std::bad_alloc);
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_TRUE(canceled);
}

TEST(Operation, ResultMoveCanReenterCancellation) {
    struct ReenterMove {
        int *moves;
        std::function<void()> *on_move;
        ReenterMove(int *count, std::function<void()> *callback)
            : moves(count), on_move(callback) {}
        ReenterMove(ReenterMove &&other) noexcept : moves(other.moves), on_move(other.on_move) {
            if (++*moves == 3) {
                (*on_move)();
            }
        }
    };
    int moves = 0;
    bool canceled = false;
    std::function<void()> on_move;
    std::vector<std::function<void()>> tasks;
    CompletionSource<ReenterMove> source([&](auto task) { tasks.push_back(std::move(task)); },
                                        [&](auto result) {
                                            canceled = std::holds_alternative<OperationError>(result);
                                        });
    auto operation = source.operation();
    on_move = [&] { EXPECT_TRUE(operation.cancel()); };
    OperationResult<ReenterMove> result(std::in_place_type<ReenterMove>, &moves, &on_move);
    EXPECT_FALSE(source.complete(std::move(result)));
    ASSERT_EQ(moves, 3);
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_TRUE(canceled);
}

TEST(Operation, UnitSupportsCompletionWithoutAValue) {
    std::vector<std::function<void()>> tasks;
    std::optional<OperationResult<Unit>> result;
    CompletionSource<Unit> source([&](auto task) { tasks.push_back(std::move(task)); },
                                  [&](auto value) { result = std::move(value); });
    EXPECT_TRUE(source.complete(Unit{}));
    EXPECT_FALSE(source.operation().cancel());
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::holds_alternative<Unit>(*result));
}

TEST(Operation, ConcurrentCancelAndCompleteHaveOneWinner) {
    std::vector<std::function<void()>> tasks;
    std::atomic<int> callbacks{0};
    CompletionSource<int> source([&](auto task) { tasks.push_back(std::move(task)); },
                                 [&](auto) { ++callbacks; });
    auto handle = source.operation();
    std::atomic<int> winners{0};
    std::thread first([&] { winners += source.complete(5) ? 1 : 0; });
    std::thread second([&] { winners += handle.cancel() ? 1 : 0; });
    first.join();
    second.join();
    EXPECT_EQ(winners, 1);
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    EXPECT_EQ(callbacks, 1);
}

TEST(Operation, QueuedMoveOnlyResultOutlivesHandlesAndAllowsReentry) {
    std::vector<std::function<void()>> tasks;
    std::optional<int> received;
    {
        CompletionSource<std::unique_ptr<int>> source(
            [&](auto task) { tasks.push_back(std::move(task)); },
            [&](auto result) { received = *std::get<std::unique_ptr<int>>(result); });
        EXPECT_TRUE(source.complete(std::make_unique<int>(23)));
    }
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    ASSERT_TRUE(received);
    EXPECT_EQ(*received, 23);

    bool reentered = false;
    std::optional<Operation<int>> operation;
    CompletionSource<int> source([&](auto task) { tasks.push_back(std::move(task)); },
                                 [&](auto) { reentered = !operation->cancel(); });
    operation = source.operation();
    EXPECT_TRUE(source.complete(1));
    tasks.back()();
    EXPECT_TRUE(reentered);
}
