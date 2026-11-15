#pragma once

#include "p2pdc/frame.h"
#include "p2pdc/memory_budget.h"

#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace p2pdc::detail {

struct CandidateLimits {
    std::size_t candidate_bytes;
    std::size_t mid_bytes;
    std::size_t payload_bytes;
};
struct CandidateView {
    std::string_view candidate;
    std::string_view mid;
};
enum class CandidateError { InvalidRecord, TooLarge, Full, AllocationFailed, Closed };
template <class T> using CandidateResult = std::variant<T, CandidateError>;
inline constexpr std::size_t candidate_header_size = 9;

// Payload is versioned and length-delimited. Views borrow the input storage.
// Empty/end-of-candidates markers are not represented in this provisional format.
[[nodiscard]] CandidateResult<Bytes> encode_candidate(CandidateView candidate,
                                                      CandidateLimits limits);
[[nodiscard]] CandidateResult<CandidateView> decode_candidate(ByteView payload,
                                                              CandidateLimits limits);

struct BufferedCandidate {
    std::string candidate;
    std::string mid;
    MemoryReservation reservation;
    std::optional<MemoryReservation> extra;
};

// Only pass payloads from successfully verified candidate envelopes. Methods
// are serialized. Reservations cover retained string capacities, excluding
// allocator overhead and transient excess before capacity is reconciled.
// On Full or AllocationFailed the owner must fail the attempt or retain the
// verified payload for retry; the replay guard has already accepted its sequence.
class CandidateBuffer {
  public:
    CandidateBuffer(CandidateLimits limits, MemoryBudget budget, std::size_t max_bytes,
                    std::size_t max_candidates);
    [[nodiscard]] CandidateResult<std::monostate> push(ByteView verified_payload);
    [[nodiscard]] bool mark_description_applied();
    [[nodiscard]] std::optional<BufferedCandidate> pop();
    void close();
    [[nodiscard]] std::size_t size() const;

  private:
    [[nodiscard]] CandidateResult<std::monostate> store(CandidateView view,
                                                        MemoryReservation reservation);
    CandidateLimits limits_;
    MemoryBudget budget_;
    std::size_t max_bytes_;
    std::size_t max_candidates_;
    std::size_t bytes_ = 0;
    std::deque<BufferedCandidate> candidates_;
    bool description_applied_ = false;
    bool closed_ = false;
};

} // namespace p2pdc::detail
