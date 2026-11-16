#include "../src/discovery/native_store.hpp"

#ifdef P2PDC_TEST_SECURITY
#include "p2pdc/advertisement.h"
#include "p2pdc/discovery_key.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
constexpr std::array<std::byte, 20> key{std::byte{1}};
constexpr std::array<std::byte, 20> missing_key{std::byte{2}};
constexpr StoreLimits limits{1024, 8, 4096};

template <class T> T await(std::future<T> &future) {
    if (future.wait_for(15s) != std::future_status::ready) {
        throw std::runtime_error("timed out waiting for local OpenDHT callback");
    }
    return future.get();
}

class NativeStoreTest : public ::testing::Test {
  protected:
    std::shared_ptr<dht::DhtRunner> first = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> second = std::make_shared<dht::DhtRunner>();

    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345679;
        config.peer_discovery = false;
        config.peer_publish = false;
        first->run("127.0.0.1", "::1", "0", config);
        second->run("127.0.0.1", "::1", "0", config);
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        second->bootstrap(first->getBound(), [promise](bool ok) { promise->set_value(ok); });
        ASSERT_TRUE(await(future));
    }

    void TearDown() override {
        first->join();
        second->join();
    }

    static bool put(const NativeStore &store, ByteView payload) {
        return put_at(store, key, payload);
    }

    static bool put_at(const NativeStore &store, ByteView lookup_key, ByteView payload) {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        const auto error =
            store.put(lookup_key, payload, [promise](bool ok) { promise->set_value(ok); });
        if (error) {
            throw std::runtime_error("unexpected local put validation failure");
        }
        return await(future);
    }

    static LookupResult lookup(const NativeStore &store, ByteView lookup_key = key) {
        auto promise = std::make_shared<std::promise<LookupResult>>();
        auto future = promise->get_future();
        const auto error = store.lookup(
            lookup_key, [promise](LookupResult value) { promise->set_value(std::move(value)); });
        if (error) {
            throw std::runtime_error("unexpected local lookup validation failure");
        }
        return await(future);
    }
};
} // namespace

#ifdef P2PDC_TEST_SECURITY
TEST_F(NativeStoreTest, ExchangesVerifiedAdvertisementThroughStore) {
    NativeStore writer(first, limits), reader(second, limits);
    auto identity = std::get<Identity>(Identity::generate());
    auto capability = std::get<DiscoveryCapability>(generate_discovery_capability());
    auto lookup_key = std::get<DiscoveryKey>(derive_discovery_key(capability));
    AdvertisementFields fields{};
    fields.issued_at = 100;
    fields.expires_at = 200;
    fields.instance_id[0] = std::byte{3};
    fields.recipient_public_key[0] = std::byte{4};
    AdvertisementPolicy policy{150, 100, 0};
    auto encoded = encode_advertisement(identity, fields, capability, policy);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    ASSERT_TRUE(put_at(writer, lookup_key, std::get<Bytes>(encoded)));
    auto received = lookup(reader, lookup_key);
    ASSERT_TRUE(received.backend_success);
    ASSERT_FALSE(received.batch.error);
    ASSERT_EQ(received.batch.values.size(), 1U);
    auto decoded = decode_advertisement(received.batch.values.front(), capability, policy);
    ASSERT_TRUE(std::holds_alternative<Advertisement>(decoded));
    EXPECT_EQ(std::get<Advertisement>(decoded).fields, fields);
    capability[0] ^= std::byte{1};
    EXPECT_EQ(std::get<CryptoError>(
                  decode_advertisement(received.batch.values.front(), capability, policy)),
              CryptoError::AuthenticationFailed);
}
#endif

TEST(NativeStoreValidation, RejectsMissingRunnerAndInvalidLimits) {
    EXPECT_THROW((NativeStore{nullptr, limits}), std::invalid_argument);
    auto runner = std::make_shared<dht::DhtRunner>();
    EXPECT_THROW((NativeStore{runner, {1024, 0, 1024}}), std::invalid_argument);
    EXPECT_THROW((NativeStore{runner, {dht::MAX_VALUE_SIZE + 1, 1, 1024}}), std::invalid_argument);
    NativeStore store(runner, limits);
    EXPECT_EQ(store.put(key, {}, [](bool) {}), StoreError::NotRunning);
    EXPECT_EQ(store.lookup(key, [](LookupResult) {}), StoreError::NotRunning);
    EXPECT_EQ(std::get<StoreError>(store.watch(key, [](StoreBatch) {})), StoreError::NotRunning);
}

TEST_F(NativeStoreTest, ValidatesKeysCallbacksAndPayloadBoundsBeforeSubmission) {
    NativeStore store(first, limits);
    std::atomic<int> calls{0};
    auto completion = [&calls](bool) { ++calls; };
    EXPECT_EQ(store.put({}, {}, completion), StoreError::InvalidKey);
    EXPECT_EQ(store.put(Bytes(19), {}, completion), StoreError::InvalidKey);
    EXPECT_EQ(store.put(Bytes(21), {}, completion), StoreError::InvalidKey);
    EXPECT_EQ(store.put(Bytes(20), {}, completion), StoreError::InvalidKey);
    EXPECT_EQ(store.put(key, {}, {}), StoreError::InvalidCallback);
    EXPECT_EQ(store.lookup(key, {}), StoreError::InvalidCallback);
    EXPECT_EQ(std::get<StoreError>(store.watch(key, {})), StoreError::InvalidCallback);
    EXPECT_EQ(store.lookup({}, [](LookupResult) {}), StoreError::InvalidKey);
    EXPECT_EQ(std::get<StoreError>(store.watch({}, [](StoreBatch) {})), StoreError::InvalidKey);
    EXPECT_EQ(store.put(key, Bytes(1025), completion), StoreError::ResourceLimit);
    NativeStore tiny(first, {1024, 1, 1});
    EXPECT_EQ(tiny.put(key, Bytes(2), completion), StoreError::ResourceLimit);
    EXPECT_EQ(calls, 0);
}

TEST_F(NativeStoreTest, PreservesBinaryEmptyAndRepeatedImmutableRecords) {
    NativeStore writer(first, limits), reader(second, limits);
    const Bytes payload{std::byte{0}, std::byte{255}, std::byte{42}};
    ASSERT_TRUE(put(writer, payload));
    ASSERT_TRUE(put(writer, payload));
    ASSERT_TRUE(put(writer, {}));
    const auto result = lookup(reader);
    EXPECT_TRUE(result.backend_success);
    EXPECT_FALSE(result.batch.error);
    EXPECT_EQ(std::count(result.batch.values.begin(), result.batch.values.end(), payload), 2);
    EXPECT_EQ(std::count(result.batch.values.begin(), result.batch.values.end(), Bytes{}), 1);
    const auto missing = lookup(reader, missing_key);
    EXPECT_TRUE(missing.backend_success);
    EXPECT_TRUE(missing.batch.values.empty());
    EXPECT_FALSE(missing.batch.error);
}

TEST_F(NativeStoreTest, DiscoveryLookupCollapsesIdenticalPublicationsAcrossIds) {
    NativeStore writer(first, limits), reader(second, {1024, 1, 1024});
    const Bytes payload{std::byte{7}, std::byte{8}};
    ASSERT_TRUE(put(writer, payload));
    ASSERT_TRUE(put(writer, payload));
    const auto distinct = lookup(reader);
    EXPECT_FALSE(distinct.batch.error);
    EXPECT_EQ(distinct.batch.values, (std::vector<Bytes>{payload}));
    EXPECT_EQ(distinct.batch.dropped, 1U);
    auto promise = std::make_shared<std::promise<LookupResult>>();
    auto future = promise->get_future();
    ASSERT_FALSE(reader.lookup(
        key, [promise](LookupResult value) { promise->set_value(std::move(value)); },
        LookupDeduplication::Content));
    const auto result = await(future);
    ASSERT_TRUE(result.backend_success);
    ASSERT_FALSE(result.batch.error);
    EXPECT_EQ(result.batch.values, (std::vector<Bytes>{payload}));
}

TEST_F(NativeStoreTest, LookupDropsRecordsBeyondCountIndividualAndAggregateBounds) {
    NativeStore writer(first, limits);
    ASSERT_TRUE(put(writer, Bytes(2, std::byte{1})));
    ASSERT_TRUE(put(writer, Bytes(2, std::byte{2})));
    ASSERT_TRUE(put(writer, Bytes(2, std::byte{3})));
    for (const auto bound :
         {StoreLimits{8, 1, 100}, StoreLimits{8, 8, 2}, StoreLimits{1, 8, 100}}) {
        NativeStore reader(second, bound);
        const auto result = lookup(reader);
        EXPECT_FALSE(result.batch.error);
        EXPECT_LE(result.batch.values.size(), 1U);
        EXPECT_GE(result.batch.dropped, 2U);
        std::size_t bytes = 0;
        for (const auto &value : result.batch.values) {
            EXPECT_LE(value.size(), bound.max_value_bytes);
            bytes += value.size();
        }
        EXPECT_LE(bytes, bound.max_total_bytes);
    }
}

TEST_F(NativeStoreTest, WatchDeliversBinaryAndCancelSuppressesLaterPublications) {
    NativeStore writer(first, limits), reader(second, limits);
    auto promise = std::make_shared<std::promise<StoreBatch>>();
    auto once = std::make_shared<std::atomic_bool>(false);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto future = promise->get_future();
    auto result = reader.watch(key, [promise, once, calls](StoreBatch batch) {
        ++*calls;
        if (!once->exchange(true)) {
            promise->set_value(std::move(batch));
        }
    });
    ASSERT_TRUE(std::holds_alternative<NativeWatch>(result));
    auto watch = std::get<NativeWatch>(std::move(result));
    ASSERT_EQ(watch.registration().wait_for(15s), std::future_status::ready);
    ASSERT_NE(watch.registration().get(), 0U);
    const Bytes payload{std::byte{0}, std::byte{255}};
    ASSERT_TRUE(put(writer, payload));
    const auto batch = await(future);
    ASSERT_FALSE(batch.error);
    EXPECT_FALSE(batch.expired);
    ASSERT_FALSE(batch.values.empty());
    EXPECT_EQ(batch.values.front(), payload);
    watch.cancel();
    watch.cancel();
    const auto count = calls->load();
    ASSERT_TRUE(put(writer, Bytes{std::byte{9}}));
    EXPECT_FALSE(lookup(reader).batch.error);
    EXPECT_EQ(calls->load(), count);
}

TEST_F(NativeStoreTest, WatchDropsOversizedRecordAndImmediateDestructionSuppressesCallbacks) {
    NativeStore writer(first, limits), reader(second, {1, 8, 8});
    std::mutex mutex;
    std::vector<StoreBatch> batches;
    auto result = reader.watch(key, [&](StoreBatch batch) {
        std::lock_guard lock(mutex);
        batches.push_back(std::move(batch));
    });
    auto watch = std::get<NativeWatch>(std::move(result));
    ASSERT_EQ(watch.registration().wait_for(15s), std::future_status::ready);
    ASSERT_TRUE(put(writer, Bytes(2)));
    ASSERT_TRUE(put(writer, Bytes{std::byte{7}}));
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    auto delivered = [&] {
        std::lock_guard lock(mutex);
        return std::any_of(batches.begin(), batches.end(), [](const StoreBatch &batch) {
            return !batch.values.empty() && batch.values.front() == Bytes{std::byte{7}};
        });
    };
    while (!delivered() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    ASSERT_TRUE(delivered());
    watch.cancel();
    std::size_t dropped = 0;
    for (const auto &batch : batches) {
        EXPECT_FALSE(batch.error);
        dropped += batch.dropped;
        for (const auto &value : batch.values)
            EXPECT_LE(value.size(), 1U);
    }
    EXPECT_GE(dropped, 1U);
    std::atomic<int> calls{0};
    {
        auto ignored = reader.watch(missing_key, [&calls](StoreBatch) { ++calls; });
        ASSERT_TRUE(std::holds_alternative<NativeWatch>(ignored));
    }
    auto published = std::make_shared<std::promise<bool>>();
    auto published_future = published->get_future();
    ASSERT_FALSE(writer.put(missing_key, Bytes{std::byte{3}},
                            [published](bool ok) { published->set_value(ok); }));
    ASSERT_TRUE(await(published_future));
    EXPECT_FALSE(lookup(reader, missing_key).batch.values.empty());
    EXPECT_EQ(calls, 0);
}

TEST_F(NativeStoreTest, ExplicitCancelWaitsForRunningCallback) {
    NativeStore writer(first, limits), reader(second, limits);
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    std::promise<void> release;
    auto unblock = release.get_future().share();
    auto once = std::make_shared<std::atomic_bool>(false);
    auto result = reader.watch(key, [entered, unblock, once](StoreBatch) {
        if (!once->exchange(true)) {
            entered->set_value();
            unblock.wait();
        }
    });
    auto watch = std::get<NativeWatch>(std::move(result));
    ASSERT_FALSE(writer.put(key, Bytes{std::byte{1}}, [](bool) {}));
    ASSERT_EQ(entered_future.wait_for(15s), std::future_status::ready);
    auto canceled = std::async(std::launch::async, [&watch] { watch.cancel(); });
    EXPECT_EQ(canceled.wait_for(20ms), std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(canceled.wait_for(15s), std::future_status::ready);
    canceled.get();
}

TEST_F(NativeStoreTest, DestructionDoesNotWaitForRunningCallback) {
    NativeStore writer(first, limits), reader(second, limits);
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    std::promise<void> release;
    auto unblock = release.get_future().share();
    auto once = std::make_shared<std::atomic_bool>(false);
    auto result = reader.watch(key, [entered, unblock, once](StoreBatch) {
        if (!once->exchange(true)) {
            entered->set_value();
            unblock.wait();
        }
    });
    std::optional<NativeWatch> watch(std::get<NativeWatch>(std::move(result)));
    ASSERT_FALSE(writer.put(key, Bytes{std::byte{1}}, [](bool) {}));
    ASSERT_EQ(entered_future.wait_for(15s), std::future_status::ready);
    auto dropped = std::async(std::launch::async, [&watch] { watch.reset(); });
    EXPECT_EQ(dropped.wait_for(2s), std::future_status::ready);
    release.set_value();
    dropped.get();
}

TEST(NativeStoreValidation, ReportsFailedBackendLookupOnAnIsolatedRunner) {
    auto runner = std::make_shared<dht::DhtRunner>();
    dht::DhtRunner::Config config;
    config.dht_config.node_config.network = 0x12345679;
    config.peer_discovery = false;
    config.peer_publish = false;
    runner->run("127.0.0.1", "::1", "0", config);
    NativeStore store(runner, limits);
    auto promise = std::make_shared<std::promise<LookupResult>>();
    auto future = promise->get_future();
    ASSERT_FALSE(store.lookup(
        key, [promise](LookupResult result) { promise->set_value(std::move(result)); }));
    const auto result = await(future);
    EXPECT_FALSE(result.backend_success);
    EXPECT_TRUE(result.batch.values.empty());
    EXPECT_FALSE(result.batch.error);
    runner->join();
}

TEST_F(NativeStoreTest, WatchCallbackCanCancelItsOwnSubscription) {
    NativeStore writer(first, limits), reader(second, limits);
    auto holder = std::make_shared<std::optional<NativeWatch>>();
    auto weak = std::weak_ptr(holder);
    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();
    auto result = reader.watch(key, [weak, completed](StoreBatch) {
        if (auto owner = weak.lock(); owner && *owner) {
            (*owner)->cancel();
            completed->set_value();
        }
    });
    holder->emplace(std::get<NativeWatch>(std::move(result)));
    ASSERT_FALSE(writer.put(key, Bytes{std::byte{1}}, [](bool) {}));
    EXPECT_EQ(future.wait_for(15s), std::future_status::ready);
    holder.reset();
}

TEST_F(NativeStoreTest, LookupAllocationFailuresKeepPrefixAndSurfaceRawEarlyStopStatus) {
    NativeStore writer(first, limits);
    ASSERT_TRUE(put(writer, Bytes(2, std::byte{1})));
    ASSERT_TRUE(put(writer, Bytes(2, std::byte{2})));
    for (const bool length_error : {false, true}) {
        auto copies = std::make_shared<std::atomic<int>>(0);
        NativeStore reader(second, limits, [copies, length_error] {
            if (copies->fetch_add(1) == 1) {
                if (length_error) {
                    throw std::length_error("injected record allocation failure");
                }
                throw std::bad_alloc{};
            }
        });
        const auto result = lookup(reader);
        EXPECT_EQ(result.batch.error, StoreError::AllocationFailed);
        ASSERT_EQ(result.batch.values.size(), 1U);
        EXPECT_EQ(result.batch.values.front().size(), 2U);
        EXPECT_TRUE(result.backend_success);
        EXPECT_EQ(copies->load(), 2);
    }
}

TEST_F(NativeStoreTest, WatchAllocationFailureIsReportedAndEndsSubscription) {
    NativeStore writer(first, limits);
    NativeStore reader(second, limits, [] { throw std::bad_alloc{}; });
    auto promise = std::make_shared<std::promise<StoreBatch>>();
    auto future = promise->get_future();
    auto result =
        reader.watch(key, [promise](StoreBatch batch) { promise->set_value(std::move(batch)); });
    auto watch = std::get<NativeWatch>(std::move(result));
    ASSERT_TRUE(put(writer, Bytes(2)));
    const auto batch = await(future);
    EXPECT_EQ(batch.error, StoreError::AllocationFailed);
    EXPECT_TRUE(batch.values.empty());
    watch.cancel();
    ASSERT_TRUE(put(writer, Bytes(2, std::byte{1})));
    EXPECT_TRUE(watch.registration().valid());
}
