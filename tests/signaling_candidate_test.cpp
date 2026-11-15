#include "../src/security/signaling_candidate.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
constexpr CandidateLimits limits{100, 20, 129};
Bytes encoded(std::string_view candidate = "candidate:1 1 udp 123 127.0.0.1 9876 typ host",
              std::string_view mid = "0") {
    return std::get<Bytes>(encode_candidate({candidate, mid}, limits));
}
} // namespace

TEST(SignalingCandidate, RoundTripsBorrowedFieldsAndRejectsInvalidRecords) {
    auto wire = encoded();
    auto decoded = decode_candidate(wire, limits);
    ASSERT_TRUE(std::holds_alternative<CandidateView>(decoded));
    const auto view = std::get<CandidateView>(decoded);
    EXPECT_EQ(view.mid, "0");
    EXPECT_EQ(view.candidate, "candidate:1 1 udp 123 127.0.0.1 9876 typ host");
    EXPECT_EQ(view.candidate.data(),
              reinterpret_cast<const char *>(wire.data() + candidate_header_size));
    EXPECT_EQ(std::get<CandidateError>(decode_candidate({}, limits)),
              CandidateError::InvalidRecord);
    wire.push_back(std::byte{0});
    EXPECT_EQ(std::get<CandidateError>(decode_candidate(wire, limits)),
              CandidateError::InvalidRecord);
    EXPECT_EQ(std::get<CandidateError>(encode_candidate({"", "0"}, limits)),
              CandidateError::InvalidRecord);
    EXPECT_EQ(
        std::get<CandidateError>(encode_candidate({std::string_view("x\0y", 3), "0"}, limits)),
        CandidateError::InvalidRecord);
    EXPECT_EQ(std::get<CandidateError>(encode_candidate({"abc", "0"}, {2, 20, 100})),
              CandidateError::TooLarge);
}

TEST(SignalingCandidate, BuffersUntilDescriptionAndRetainsReservations) {
    MemoryBudget budget(500);
    CandidateBuffer queue(limits, budget, 500, 2);
    auto first = encoded("candidate:first", "0");
    auto second = encoded("candidate:second", "1");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(queue.push(first)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(queue.push(second)));
    EXPECT_FALSE(queue.pop());
    EXPECT_GT(budget.used(), 0U);
    EXPECT_TRUE(queue.mark_description_applied());
    EXPECT_FALSE(queue.mark_description_applied());
    auto item = queue.pop();
    ASSERT_TRUE(item);
    EXPECT_EQ(item->candidate, "candidate:first");
    EXPECT_EQ(item->mid, "0");
    const auto used = budget.used();
    item.reset();
    EXPECT_LT(budget.used(), used);
    item = queue.pop();
    ASSERT_TRUE(item);
    EXPECT_EQ(item->candidate, "candidate:second");
    queue.close();
    EXPECT_GT(budget.used(), 0U);
    item.reset();
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(std::get<CandidateError>(queue.push(first)), CandidateError::Closed);
}

TEST(SignalingCandidate, EnforcesCountAndSharedBudgetWithoutPartialAdmission) {
    MemoryBudget budget(100);
    CandidateBuffer first(limits, budget, 100, 1), second(limits, budget, 100, 1);
    const auto record = encoded("candidate:first");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.push(record)));
    EXPECT_EQ(std::get<CandidateError>(first.push(record)), CandidateError::Full);
    auto external = budget.try_reserve(budget.limit() - budget.used());
    ASSERT_TRUE(external);
    EXPECT_EQ(std::get<CandidateError>(second.push(record)), CandidateError::Full);
    EXPECT_EQ(second.size(), 0U);
    external.reset();
    first.close();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(second.push(record)));
    EXPECT_THROW((CandidateBuffer{limits, budget, 100, 0}), std::invalid_argument);
}
