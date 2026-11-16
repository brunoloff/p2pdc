#include "../src/discovery/native_store.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
std::shared_ptr<dht::Value> record(dht::Value::Id id, std::uint8_t byte) {
    auto result = std::make_shared<dht::Value>(dht::Blob{byte});
    result->id = id;
    return result;
}
} // namespace

TEST(NativeStoreLookupAccumulator, SuppressesExactRepeatsButPreservesDistinctRecords) {
    LookupAccumulator accumulator({4, 3, 3}, {});
    ASSERT_TRUE(accumulator.receive({record(1, 10), record(1, 10)}));
    ASSERT_TRUE(accumulator.receive({record(1, 10), record(1, 20), record(2, 10)}));
    ASSERT_EQ(accumulator.batch.values.size(), 3U);
    EXPECT_EQ(accumulator.batch.values[0], (Bytes{std::byte{10}}));
    EXPECT_EQ(accumulator.batch.values[1], (Bytes{std::byte{20}}));
    EXPECT_EQ(accumulator.batch.values[2], (Bytes{std::byte{10}}));
    EXPECT_FALSE(accumulator.batch.error);
    EXPECT_TRUE(accumulator.receive({record(2, 10)}));
    EXPECT_TRUE(accumulator.receive({record(3, 10)}));
    EXPECT_FALSE(accumulator.batch.error);
    EXPECT_EQ(accumulator.batch.values.size(), 3U);
    EXPECT_EQ(accumulator.batch.dropped, 1U);
}

TEST(NativeStoreLookupAccumulator, InvalidIdsAreNotAssumedToIdentifyTheSameRecord) {
    LookupAccumulator accumulator({4, 1, 4}, {});
    ASSERT_TRUE(accumulator.receive({record(dht::Value::INVALID_ID, 10)}));
    EXPECT_TRUE(accumulator.receive({record(dht::Value::INVALID_ID, 10)}));
    EXPECT_FALSE(accumulator.batch.error);
    EXPECT_EQ(accumulator.batch.values.size(), 1U);
    EXPECT_EQ(accumulator.batch.dropped, 1U);
}

TEST(NativeStoreLookupAccumulator, OversizedRecordIsDroppedWithoutEndingLookup) {
    LookupAccumulator accumulator({1, 4, 8}, {});
    auto oversized = std::make_shared<dht::Value>(dht::Blob{1, 2});
    oversized->id = 5;
    EXPECT_TRUE(accumulator.receive({oversized, record(6, 30)}));
    EXPECT_FALSE(accumulator.batch.error);
    EXPECT_EQ(accumulator.batch.values, (std::vector<Bytes>{Bytes{std::byte{30}}}));
    EXPECT_EQ(accumulator.batch.dropped, 1U);
}

TEST(NativeStoreLookupAccumulator, ExactRepeatDoesNotCopyRecordAgain) {
    unsigned copies = 0;
    LookupAccumulator accumulator({4, 2, 4}, [&] { ++copies; });
    ASSERT_TRUE(accumulator.receive({record(1, 10)}));
    ASSERT_TRUE(accumulator.receive({record(1, 10)}));
    EXPECT_EQ(copies, 1U);
    ASSERT_TRUE(accumulator.receive({record(1, 20)}));
    EXPECT_EQ(copies, 2U);
}

TEST(NativeStoreLookupAccumulator, RejectsInvalidRecordsAndRetainsBoundedPrefix) {
    LookupAccumulator accumulator({4, 2, 2}, {});
    EXPECT_FALSE(accumulator.receive({record(1, 10), nullptr}));
    ASSERT_EQ(accumulator.batch.values.size(), 1U);
    EXPECT_EQ(accumulator.batch.error, StoreError::InvalidRecord);
    EXPECT_FALSE(accumulator.receive({record(2, 20)}));
}

TEST(NativeStoreLookupAccumulator, ContentDeduplicationPreservesDistinctPayloads) {
    LookupAccumulator accumulator({4, 2, 8}, {}, LookupDeduplication::Content);
    ASSERT_TRUE(accumulator.receive(
        {record(1, 10), record(2, 10), record(dht::Value::INVALID_ID, 10), record(1, 20)}));
    ASSERT_EQ(accumulator.batch.values.size(), 2U);
    EXPECT_EQ(accumulator.batch.values[0], (Bytes{std::byte{10}}));
    EXPECT_EQ(accumulator.batch.values[1], (Bytes{std::byte{20}}));
    EXPECT_TRUE(accumulator.receive({record(3, 30)}));
    EXPECT_FALSE(accumulator.batch.error);
    EXPECT_EQ(accumulator.batch.dropped, 1U);
}

TEST(NativeStoreLookupAccumulator, ContentDeduplicationBoundsDuplicateInspection) {
    LookupAccumulator accumulator({4, 1, 4}, {}, LookupDeduplication::Content);
    for (unsigned i = 0; i < 8; ++i)
        ASSERT_TRUE(accumulator.receive({record(i + 1, 10)}));
    EXPECT_TRUE(accumulator.receive({record(9, 20)}));
    EXPECT_FALSE(accumulator.batch.error);
    EXPECT_EQ(accumulator.batch.values.size(), 1U);
    EXPECT_EQ(accumulator.batch.dropped, 1U);
    EXPECT_THROW((LookupAccumulator({4, 1, 4}, {}, static_cast<LookupDeduplication>(99))),
                 std::invalid_argument);
}
