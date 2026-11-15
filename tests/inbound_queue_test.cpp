#include "../src/transport/inbound_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <stdexcept>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(InboundQueue, RejectsInvalidTypeOnceAndReleasesQueuedMessages) {
    MemoryBudget budget(4);
    InboundQueue queue(budget, 4, 2, 4);
    EXPECT_TRUE(queue.push(Bytes{std::byte{1}}).schedule_drain);
    const auto rejection = queue.reject_invalid_type();
    EXPECT_EQ(rejection.status, InboundStatus::InvalidType);
    EXPECT_FALSE(rejection.schedule_drain);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::InvalidType);
    EXPECT_EQ(queue.reject_invalid_type().status, InboundStatus::Closed);
    InboundQueue empty(budget, 4, 2, 4);
    EXPECT_TRUE(empty.reject_invalid_type().schedule_drain);
}

TEST(InboundQueue, PreservesOrderAndReservationsThroughDelivery) {
    MemoryBudget budget(8);
    InboundQueue queue(budget, 8, 4, 4);
    auto first = queue.push(Bytes{std::byte{1}, std::byte{2}});
    EXPECT_EQ(first.status, InboundStatus::Accepted);
    EXPECT_TRUE(first.schedule_drain);
    auto second = queue.push(Bytes{std::byte{3}});
    EXPECT_FALSE(second.schedule_drain);
    EXPECT_EQ(queue.buffered_bytes(), 3U);
    auto next = queue.next();
    ASSERT_TRUE(std::holds_alternative<InboundMessage>(next));
    EXPECT_EQ(std::get<InboundMessage>(next).payload, (Bytes{std::byte{1}, std::byte{2}}));
    EXPECT_EQ(budget.used(), 3U);
    next = queue.next();
    EXPECT_EQ(std::get<InboundMessage>(next).payload, (Bytes{std::byte{3}}));
    EXPECT_EQ(budget.used(), 1U);
    next = queue.next();
    EXPECT_EQ(std::get<InboundStatus>(next), InboundStatus::Idle);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_TRUE(queue.push(Bytes{}).schedule_drain);
}

TEST(InboundQueue, OverflowClearsQueuedBytesAndSignalsOnce) {
    MemoryBudget budget(4);
    InboundQueue queue(budget, 4, 1, 4);
    EXPECT_TRUE(queue.push(Bytes{std::byte{1}}).schedule_drain);
    const auto overflow = queue.push(Bytes{});
    EXPECT_EQ(overflow.status, InboundStatus::Full);
    EXPECT_FALSE(overflow.schedule_drain);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::Full);
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::Closed);
    EXPECT_EQ(queue.push(Bytes{}).status, InboundStatus::Closed);
}

TEST(InboundQueue, OptionalContentDeduplicationOnlySkipsQueuedCopies) {
    MemoryBudget budget(8);
    InboundQueue queue(budget, 8, 1, 4);
    EXPECT_TRUE(queue.push(Bytes{std::byte{1}}, true).schedule_drain);
    for (int i = 0; i < 10; ++i) {
        const auto duplicate = queue.push(Bytes{std::byte{1}}, true);
        EXPECT_EQ(duplicate.status, InboundStatus::Accepted);
        EXPECT_FALSE(duplicate.schedule_drain);
    }
    EXPECT_EQ(queue.buffered_bytes(), 1U);
    ASSERT_TRUE(std::holds_alternative<InboundMessage>(queue.next()));
    EXPECT_EQ(queue.push(Bytes{std::byte{1}}, true).status, InboundStatus::Accepted);
    EXPECT_EQ(queue.push(Bytes{std::byte{2}}, true).status, InboundStatus::Full);
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::Full);
    InboundQueue plain(budget, 8, 1, 4);
    EXPECT_EQ(plain.push(Bytes{std::byte{1}}).status, InboundStatus::Accepted);
    EXPECT_EQ(plain.push(Bytes{std::byte{1}}).status, InboundStatus::Full);
}

TEST(InboundQueue, EnforcesAggregateCapacityAndValidatesLimits) {
    MemoryBudget budget(2);
    EXPECT_THROW((InboundQueue{budget, 1, 0, 1}), std::invalid_argument);
    EXPECT_THROW((InboundQueue{budget, 1, 1, 2}), std::invalid_argument);
    InboundQueue first(budget, 4, 2, 4), second(budget, 4, 2, 4);
    EXPECT_EQ(first.push(Bytes{std::byte{1}, std::byte{2}}).status, InboundStatus::Accepted);
    EXPECT_EQ(second.push(Bytes{std::byte{3}}).status, InboundStatus::Full);
    EXPECT_EQ(std::get<InboundStatus>(second.next()), InboundStatus::Full);
    first.close();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(std::get<InboundStatus>(first.next()), InboundStatus::Closed);
    InboundQueue third(budget, 4, 2, 1);
    EXPECT_EQ(third.push(Bytes{std::byte{1}, std::byte{2}}).status, InboundStatus::TooLarge);
    EXPECT_EQ(std::get<InboundStatus>(third.next()), InboundStatus::TooLarge);
}

TEST(InboundQueue, ConcurrentProducersScheduleOnlyOneDrain) {
    MemoryBudget budget(4);
    InboundQueue queue(budget, 4, 4, 1);
    std::atomic<int> schedules{0};
    auto produce = [&] {
        for (int i = 0; i < 2; ++i) {
            auto result = queue.push(Bytes{std::byte{1}});
            EXPECT_EQ(result.status, InboundStatus::Accepted);
            if (result.schedule_drain) {
                ++schedules;
            }
        }
    };
    std::thread first(produce), second(produce);
    first.join();
    second.join();
    EXPECT_EQ(schedules, 1);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(std::holds_alternative<InboundMessage>(queue.next()));
    }
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::Idle);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(InboundQueue, AccountsCapacityAndPreservesOutstandingReservationOnClose) {
    MemoryBudget budget(128);
    InboundQueue queue(budget, 128, 2, 2);
    Bytes oversized_capacity{std::byte{1}};
    oversized_capacity.reserve(129);
    EXPECT_EQ(queue.push(std::move(oversized_capacity)).status, InboundStatus::Full);
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::Full);
    InboundQueue second(budget, 128, 2, 2);
    Bytes message{std::byte{2}};
    message.reserve(4);
    const auto allocated = message.capacity();
    EXPECT_EQ(second.push(std::move(message)).status, InboundStatus::Accepted);
    auto in_flight = second.next();
    ASSERT_TRUE(std::holds_alternative<InboundMessage>(in_flight));
    EXPECT_EQ(budget.used(), allocated);
    second.close();
    EXPECT_EQ(budget.used(), allocated);
    in_flight = InboundStatus::Closed;
    EXPECT_EQ(budget.used(), 0U);
}

TEST(InboundQueue, ProducerRacingIdleTransitionNeverLosesDrain) {
    MemoryBudget budget(8);
    InboundQueue queue(budget, 8, 4, 2);
    EXPECT_TRUE(queue.push(Bytes{std::byte{1}}).schedule_drain);
    auto first = queue.next();
    ASSERT_TRUE(std::holds_alternative<InboundMessage>(first));
    std::barrier start(2);
    InboundPush pushed{InboundStatus::Closed, false};
    std::thread producer([&] {
        start.arrive_and_wait();
        pushed = queue.push(Bytes{std::byte{2}});
    });
    start.arrive_and_wait();
    auto next = queue.next();
    producer.join();
    EXPECT_EQ(pushed.status, InboundStatus::Accepted);
    if (std::holds_alternative<InboundStatus>(next)) {
        EXPECT_EQ(std::get<InboundStatus>(next), InboundStatus::Idle);
        EXPECT_TRUE(pushed.schedule_drain);
        next = queue.next();
    } else {
        EXPECT_FALSE(pushed.schedule_drain);
    }
    ASSERT_TRUE(std::holds_alternative<InboundMessage>(next));
    EXPECT_EQ(std::get<InboundMessage>(next).payload, (Bytes{std::byte{2}}));
    EXPECT_EQ(std::get<InboundStatus>(queue.next()), InboundStatus::Idle);
}
