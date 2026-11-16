#include "../src/discovery/native_store.hpp"
#include "../src/discovery/native_mailbox.hpp"
#include "../src/discovery/native_outbox.hpp"
#include "../src/discovery/native_publisher.hpp"
#include "../src/discovery/signaling_queue.hpp"
#ifdef P2PDC_TEST_SECURITY
#include "../src/security/signaling_session.hpp"
#endif

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
constexpr std::array<std::byte, 20> address{std::byte{21}};
constexpr StoreLimits store_limits{1024, 16, 16384};

class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    void drain() {
        for (int count = 0; count < 1000; ++count) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        drain();
        return done();
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

class NativeMailboxTest : public ::testing::Test {
  protected:
    std::shared_ptr<dht::DhtRunner> first = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> second = std::make_shared<dht::DhtRunner>();
    Loop loop;
    std::vector<Bytes> records;
    std::vector<MailboxError> errors;
    std::optional<bool> caught_up;

    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345681;
        config.peer_discovery = false;
        config.peer_publish = false;
        first->run("127.0.0.1", "::1", "0", config);
        second->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        second->bootstrap(first->getBound(), [done](bool value) { done->set_value(value); });
        ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(future.get());
    }
    void TearDown() override {
        first->join();
        second->join();
    }
    MailboxCallbacks callbacks() {
        return {[this](Bytes value) { records.push_back(std::move(value)); },
                [this](MailboxError error, std::optional<StoreError>) { errors.push_back(error); },
                [this](bool success) { caught_up = success; }};
    }
    bool put(ByteView value) {
        NativeStore store(first, store_limits);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        if (store.put(address, value, [done](bool success) { done->set_value(success); })) {
            return false;
        }
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
    std::unique_ptr<NativeMailbox> create(MailboxLimits limits, MemoryBudget budget) {
        auto result = NativeMailbox::create(NativeStore(second, store_limits), address, limits,
                                            std::move(budget), loop.executor(), callbacks());
        return std::move(std::get<std::unique_ptr<NativeMailbox>>(result));
    }
};
} // namespace

TEST_F(NativeMailboxTest, ReceivesExistingAndNewValuesAndStopsAfterClose) {
    const Bytes before{std::byte{1}, std::byte{0}};
    const Bytes after{std::byte{2}, std::byte{255}};
    ASSERT_TRUE(put(before));
    auto mailbox = create({4096, 16, 1024}, MemoryBudget(4096));
    EXPECT_TRUE(mailbox->is_open());
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mailbox->catch_up()));
    EXPECT_EQ(std::get<CatchupError>(mailbox->catch_up()), CatchupError::AlreadyStarted);
    ASSERT_TRUE(loop.until([&] { return caught_up.has_value() && !records.empty(); }));
    EXPECT_TRUE(*caught_up);
    EXPECT_NE(std::find(records.begin(), records.end(), before), records.end());
    ASSERT_TRUE(put(after));
    ASSERT_TRUE(loop.until(
        [&] { return std::find(records.begin(), records.end(), after) != records.end(); }));
    EXPECT_TRUE(errors.empty());
    mailbox->close();
    EXPECT_FALSE(mailbox->is_open());
    EXPECT_EQ(std::get<CatchupError>(mailbox->catch_up()), CatchupError::Closed);
    const auto delivered = records.size();
    ASSERT_TRUE(put(Bytes{std::byte{3}}));
    loop.drain();
    EXPECT_EQ(records.size(), delivered);
}

TEST_F(NativeMailboxTest, QueueOverflowDropsExcessRecordsAndStaysOpen) {
    MemoryBudget budget(4096);
    auto mailbox = create({4096, 1, 1024}, budget);
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    ASSERT_TRUE(put(Bytes{std::byte{1}}));
    const auto first_deadline = std::chrono::steady_clock::now() + 10s;
    while (budget.used() == 0 && std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_GT(budget.used(), 0U);
    ASSERT_TRUE(put(Bytes{std::byte{2}}));
    ASSERT_TRUE(loop.until([&] { return !records.empty(); }));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(mailbox->is_open());
    EXPECT_EQ(records.front(), Bytes{std::byte{1}});
    const Bytes later{std::byte{3}};
    ASSERT_TRUE(put(later));
    ASSERT_TRUE(loop.until(
        [&] { return std::find(records.begin(), records.end(), later) != records.end(); }));
    EXPECT_TRUE(errors.empty());
}

TEST_F(NativeMailboxTest, OversizedRecordIsDroppedAndLaterRecordsArrive) {
    auto mailbox = create({4096, 16, 2}, MemoryBudget(4096));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    ASSERT_TRUE(put(Bytes(3, std::byte{9})));
    const Bytes later{std::byte{4}};
    ASSERT_TRUE(put(later));
    ASSERT_TRUE(loop.until(
        [&] { return std::find(records.begin(), records.end(), later) != records.end(); }));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(mailbox->is_open());
    EXPECT_EQ(std::count(records.begin(), records.end(), Bytes(3, std::byte{9})), 0);
}

TEST_F(NativeMailboxTest, PublisherBoundsOutstandingWritesAndPublishes) {
    auto mailbox = create({4096, 16, 1024}, MemoryBudget(4096));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    MemoryBudget budget(4);
    NativePublisher publisher(NativeStore(first, store_limits), address, budget, loop.executor());
    std::optional<bool> completed;
    auto callback = [&](bool success) { completed = success; };
    const Bytes value{std::byte{1}, std::byte{2}};
    EXPECT_EQ(std::get<PublishError>(publisher.send(Bytes(5), callback)),
              PublishError::ResourceLimit);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher.send(value, callback)));
    EXPECT_EQ(budget.used(), value.size());
    EXPECT_EQ(std::get<PublishError>(publisher.send(value, callback)), PublishError::Busy);
    ASSERT_TRUE(loop.until([&] { return completed.has_value() && !records.empty(); }));
    EXPECT_TRUE(*completed);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_NE(std::find(records.begin(), records.end(), value), records.end());
    publisher.close();
    EXPECT_FALSE(publisher.is_open());
    EXPECT_EQ(std::get<PublishError>(publisher.send(value, callback)), PublishError::Closed);
}

TEST(NativePublisherValidation, RejectsBadInputsAndReleasesBudgetOnStoreError) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, store_limits);
    MemoryBudget budget(32);
    EXPECT_THROW((NativePublisher(store, address, budget, {})), std::invalid_argument);
    NativePublisher publisher(store, address, budget, [](auto) {});
    EXPECT_THROW((void)publisher.send(Bytes(1), {}), std::invalid_argument);
    EXPECT_EQ(std::get<StoreError>(publisher.send(Bytes(1), [](bool) {})), StoreError::NotRunning);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(std::get<StoreError>(publisher.send(Bytes(1), [](bool) {})), StoreError::NotRunning);
}

TEST_F(NativeMailboxTest, PublisherAllowsReentrantSendAndSuppressesCompletionAfterClose) {
    MemoryBudget budget(2);
    NativePublisher publisher(NativeStore(first, store_limits), address, budget, loop.executor());
    int completions = 0;
    std::optional<PublishResult> second;
    const Bytes value{std::byte{7}, std::byte{8}};
    ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher.send(value, [&](bool success) {
        EXPECT_TRUE(success);
        ++completions;
        second = publisher.send(value, [&](bool) { ++completions; });
        publisher.close();
    })));
    ASSERT_TRUE(loop.until([&] { return second.has_value(); }));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(*second));
    ASSERT_TRUE(loop.until([&] { return budget.used() == 0; }));
    EXPECT_EQ(completions, 1);
    EXPECT_FALSE(publisher.is_open());
}

TEST_F(NativeMailboxTest, QueuedRecordsRemainUntilPublisherCompletes) {
    auto mailbox = create({4096, 16, 1024}, MemoryBudget(4096));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    MemoryBudget budget(4096);
    NativePublisher publisher(NativeStore(first, store_limits), address, budget, loop.executor());
    SignalingQueue queue(budget, 2048, 4, 1024);
    const Bytes first_record{std::byte{31}};
    const Bytes second_record{std::byte{32}};
    ASSERT_EQ(queue.push(first_record), SignalingQueueResult::Accepted);
    ASSERT_EQ(queue.push(second_record), SignalingQueueResult::Accepted);
    std::optional<bool> completed;
    for (int count = 0; count < 2; ++count) {
        ASSERT_TRUE(queue.front());
        completed.reset();
        ASSERT_TRUE(std::holds_alternative<std::monostate>(
            publisher.send(*queue.front(), [&](bool success) { completed = success; })));
        EXPECT_EQ(queue.size(), static_cast<std::size_t>(2 - count));
        ASSERT_TRUE(loop.until([&] { return completed.has_value(); }));
        ASSERT_TRUE(*completed);
        EXPECT_TRUE(queue.pop());
    }
    ASSERT_TRUE(loop.until([&] {
        return std::find(records.begin(), records.end(), first_record) != records.end() &&
               std::find(records.begin(), records.end(), second_record) != records.end();
    }));
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_EQ(budget.used(), 0U);
}

TEST_F(NativeMailboxTest, OutboxPublishesQueuedRecords) {
    auto mailbox = create({4096, 16, 1024}, MemoryBudget(4096));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    MemoryBudget budget(4096);
    int published = 0;
    std::vector<OutboxError> failures;
    NativeOutbox outbox(
        NativeStore(first, store_limits), address, budget, {2048, 2, 1024}, loop.executor(),
        {[&] { ++published; }, [&](OutboxError error) { failures.push_back(error); }});
    EXPECT_EQ(outbox.enqueue(Bytes{std::byte{41}}), SignalingQueueResult::Accepted);
    EXPECT_EQ(outbox.enqueue(Bytes{std::byte{42}}), SignalingQueueResult::Accepted);
    EXPECT_EQ(outbox.enqueue(Bytes{std::byte{43}}), SignalingQueueResult::Full);
    ASSERT_TRUE(loop.until([&] { return published == 2 && records.size() >= 2; }));
    EXPECT_TRUE(failures.empty());
    EXPECT_NE(std::find(records.begin(), records.end(), Bytes{std::byte{41}}), records.end());
    EXPECT_NE(std::find(records.begin(), records.end(), Bytes{std::byte{42}}), records.end());
    EXPECT_EQ(outbox.size(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_FALSE(outbox.retry());
    outbox.close();
    EXPECT_EQ(outbox.enqueue(Bytes{}), SignalingQueueResult::Closed);
}

TEST(NativeOutboxValidation, RetainsRecordAndReportsSubmissionFailure) {
    auto runner = std::make_shared<dht::DhtRunner>();
    MemoryBudget budget(32);
    std::vector<OutboxError> failures;
    NativeOutbox *outbox_pointer = nullptr;
    std::optional<bool> reentrant_retry;
    NativeOutbox outbox(NativeStore(runner, store_limits), address, budget, {16, 2, 8}, [](auto) {},
                        {[] {},
                         [&](OutboxError error) {
                             failures.push_back(error);
                             reentrant_retry = outbox_pointer->retry();
                         }});
    outbox_pointer = &outbox;
    EXPECT_EQ(outbox.enqueue(Bytes{std::byte{9}}), SignalingQueueResult::Accepted);
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(std::get<StoreError>(failures.front()), StoreError::NotRunning);
    ASSERT_TRUE(reentrant_retry.has_value());
    EXPECT_FALSE(*reentrant_retry);
    EXPECT_EQ(outbox.size(), 1U);
    EXPECT_GT(budget.used(), 0U);
    EXPECT_TRUE(outbox.retry());
    EXPECT_EQ(failures.size(), 2U);
    EXPECT_EQ(outbox.size(), 1U);
    outbox.close();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_FALSE(outbox.retry());
}

TEST(NativeOutboxValidation, RequiresHeadroomForPublicationCopy) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, store_limits);
    OutboxCallbacks callbacks{[] {}, [](OutboxError) {}};
    EXPECT_THROW((NativeOutbox(
                     store, address, MemoryBudget(10), {8, 1, 3}, [](auto) {}, callbacks)),
                 std::invalid_argument);
    EXPECT_NO_THROW(
        (NativeOutbox(store, address, MemoryBudget(11), {8, 1, 3}, [](auto) {}, callbacks)));
}

TEST(NativeMailboxValidation, RequiresCallbacksAndRunningStore) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, store_limits);
    EXPECT_THROW((NativeMailbox::create(store, address, {100, 1, 100}, MemoryBudget(100), {}, {})),
                 std::invalid_argument);
    MailboxCallbacks callbacks{[](Bytes) {}, [](MailboxError, std::optional<StoreError>) {},
                               [](bool) {}};
    EXPECT_EQ(std::get<StoreError>(NativeMailbox::create(
                  store, address, {100, 1, 100}, MemoryBudget(100), [](auto) {}, callbacks)),
              StoreError::NotRunning);
}

#ifdef P2PDC_TEST_SECURITY
TEST_F(NativeMailboxTest, DeliversAuthenticatedSignalingAndRejectsReplayAndTampering) {
    auto alice = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    auto bob = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    auto alice_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    auto bob_box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4})));
    const Bytes capability(32, std::byte{5});
    const SignalingRecipient peer_a{alice.public_key(), {std::byte{1}}};
    const SignalingRecipient peer_b{bob.public_key(), {std::byte{2}}};
    const AttemptId attempt{std::byte{3}};
    const SignalingPolicy policy{100, 60, 5, 4096};
    SignalingSession sender(peer_a, peer_b, alice_box.public_key(), bob_box.public_key(), attempt,
                            HandshakeRole::Offerer, capability, 8);
    SignalingSession receiver(peer_b, peer_a, bob_box.public_key(), alice_box.public_key(), attempt,
                              HandshakeRole::Answerer, capability, 8);
    const Bytes offer{std::byte{7}, std::byte{8}};
    auto encoded = sender.encode(alice, bob_box.public_key(), capability, SignalingKind::Offer,
                                 offer, 100, 150, policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    const auto wire = std::get<Bytes>(std::move(encoded));
    auto candidate = sender.encode(alice, bob_box.public_key(), capability,
                                   SignalingKind::Candidate, Bytes{std::byte{9}}, 100, 150, policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(candidate));
    const auto candidate_wire = std::get<Bytes>(std::move(candidate));
    MemoryBudget publish_budget(4096);
    int published = 0;
    std::vector<OutboxError> publish_errors;
    NativeOutbox outbox(
        NativeStore(first, store_limits), address, publish_budget, {2048, 4, 1024}, loop.executor(),
        {[&] { ++published; }, [&](OutboxError error) { publish_errors.push_back(error); }});
    ASSERT_EQ(outbox.enqueue(candidate_wire), SignalingQueueResult::Accepted);
    ASSERT_EQ(outbox.enqueue(wire), SignalingQueueResult::Accepted);
    ASSERT_TRUE(loop.until([&] { return published == 2 || !publish_errors.empty(); }));
    ASSERT_TRUE(publish_errors.empty());
    auto mailbox = create({4096, 16, 1024}, MemoryBudget(4096));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(mailbox->catch_up()));
    ASSERT_TRUE(loop.until([&] {
        return std::find(records.begin(), records.end(), wire) != records.end() &&
               std::find(records.begin(), records.end(), candidate_wire) != records.end();
    }));
    auto altered = wire;
    altered.back() ^= std::byte{1};
    EXPECT_FALSE(std::holds_alternative<SignalingEnvelope>(
        receiver.verify(altered, bob_box, capability, policy)));
    const auto found_candidate = std::find(records.begin(), records.end(), candidate_wire);
    const auto found_offer = std::find(records.begin(), records.end(), wire);
    ASSERT_NE(found_candidate, records.end());
    ASSERT_NE(found_offer, records.end());
    auto verified_candidate = receiver.verify(*found_candidate, bob_box, capability, policy);
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(verified_candidate));
    EXPECT_EQ(std::get<SignalingEnvelope>(verified_candidate).fields.kind,
              SignalingKind::Candidate);
    auto verified = receiver.verify(*found_offer, bob_box, capability, policy);
    ASSERT_TRUE(std::holds_alternative<SignalingEnvelope>(verified));
    EXPECT_EQ(std::get<SignalingEnvelope>(verified).fields.payload, offer);
    EXPECT_EQ(std::get<AttemptDecision>(receiver.verify(wire, bob_box, capability, policy)),
              AttemptDecision::Duplicate);
    const auto count_before_retry = std::count(records.begin(), records.end(), wire);
    ASSERT_EQ(outbox.enqueue(wire), SignalingQueueResult::Accepted);
    ASSERT_TRUE(loop.until([&] {
        return published == 3 &&
               std::count(records.begin(), records.end(), wire) > count_before_retry;
    }));
    const auto repeated = std::find(records.rbegin(), records.rend(), wire);
    ASSERT_NE(repeated, records.rend());
    EXPECT_EQ(std::get<AttemptDecision>(receiver.verify(*repeated, bob_box, capability, policy)),
              AttemptDecision::Duplicate);
    EXPECT_EQ(publish_budget.used(), 0U);
    EXPECT_TRUE(publish_errors.empty());
    EXPECT_TRUE(errors.empty());
}
#endif
#include "native_mailbox_lookup_failure_tests.hpp"
