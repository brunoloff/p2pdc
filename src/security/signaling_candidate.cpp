#include "signaling_candidate.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::array header{std::byte{'P'}, std::byte{'D'}, std::byte{'I'}, std::byte{'C'},
                            std::byte{1}};
bool has_nul(std::string_view value) { return value.find('\0') != std::string_view::npos; }
std::optional<CandidateError> validate(CandidateView candidate, CandidateLimits limits) {
    if (candidate.candidate.empty() || has_nul(candidate.candidate) || has_nul(candidate.mid)) {
        return CandidateError::InvalidRecord;
    }
    constexpr auto maximum = std::numeric_limits<std::uint16_t>::max();
    if (candidate.candidate.size() > limits.candidate_bytes ||
        candidate.mid.size() > limits.mid_bytes || candidate.candidate.size() > maximum ||
        candidate.mid.size() > maximum || limits.payload_bytes < candidate_header_size ||
        candidate.candidate.size() > limits.payload_bytes - candidate_header_size ||
        candidate.mid.size() >
            limits.payload_bytes - candidate_header_size - candidate.candidate.size()) {
        return CandidateError::TooLarge;
    }
    return std::nullopt;
}
void put_u16(Bytes &out, std::size_t value) {
    out.push_back(static_cast<std::byte>(value >> 8));
    out.push_back(static_cast<std::byte>(value & 255U));
}
std::size_t read_u16(ByteView input) {
    return (std::to_integer<std::size_t>(input[0]) << 8U) | std::to_integer<std::size_t>(input[1]);
}
std::string_view text_view(ByteView bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
void append(Bytes &out, std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text));
    out.insert(out.end(), bytes.begin(), bytes.end());
}
} // namespace

CandidateResult<Bytes> encode_candidate(CandidateView candidate, CandidateLimits limits) {
    if (auto error = validate(candidate, limits)) {
        return *error;
    }
    Bytes result(header.begin(), header.end());
    result.reserve(candidate_header_size + candidate.candidate.size() + candidate.mid.size());
    put_u16(result, candidate.candidate.size());
    put_u16(result, candidate.mid.size());
    append(result, candidate.candidate);
    append(result, candidate.mid);
    return result;
}

CandidateResult<CandidateView> decode_candidate(ByteView payload, CandidateLimits limits) {
    if (payload.size() < candidate_header_size ||
        !std::equal(header.begin(), header.end(), payload.begin())) {
        return CandidateError::InvalidRecord;
    }
    if (payload.size() > limits.payload_bytes) {
        return CandidateError::TooLarge;
    }
    const auto candidate_size = read_u16(payload.subspan(5, 2));
    const auto mid_size = read_u16(payload.subspan(7, 2));
    if (candidate_size > payload.size() - candidate_header_size ||
        mid_size != payload.size() - candidate_header_size - candidate_size) {
        return CandidateError::InvalidRecord;
    }
    CandidateView result{
        text_view(payload.subspan(candidate_header_size, candidate_size)),
        text_view(payload.subspan(candidate_header_size + candidate_size, mid_size))};
    if (auto error = validate(result, limits)) {
        return *error;
    }
    return result;
}

CandidateBuffer::CandidateBuffer(CandidateLimits limits, MemoryBudget budget, std::size_t max_bytes,
                                 std::size_t max_candidates)
    : limits_(limits), budget_(std::move(budget)), max_bytes_(max_bytes),
      max_candidates_(max_candidates) {
    if (max_candidates == 0 || limits.payload_bytes < candidate_header_size) {
        throw std::invalid_argument("invalid candidate buffer limits");
    }
}

CandidateResult<std::monostate> CandidateBuffer::push(ByteView payload) {
    if (closed_) {
        return CandidateError::Closed;
    }
    auto parsed = decode_candidate(payload, limits_);
    if (auto *error = std::get_if<CandidateError>(&parsed)) {
        return *error;
    }
    if (candidates_.size() >= max_candidates_ || payload.size() > max_bytes_ - bytes_) {
        return CandidateError::Full;
    }
    auto reservation = budget_.try_reserve(payload.size());
    if (!reservation) {
        return CandidateError::Full;
    }
    return store(std::get<CandidateView>(parsed), std::move(*reservation));
}

CandidateResult<std::monostate> CandidateBuffer::store(CandidateView view,
                                                       MemoryReservation reservation) {
    try {
        std::string candidate(view.candidate), mid(view.mid);
        if (candidate.capacity() > std::numeric_limits<std::size_t>::max() - mid.capacity()) {
            return CandidateError::Full;
        }
        const auto capacity = candidate.capacity() + mid.capacity();
        const auto charge = std::max(capacity, reservation.bytes());
        if (charge > max_bytes_ - bytes_) {
            return CandidateError::Full;
        }
        std::optional<MemoryReservation> extra;
        if (charge > reservation.bytes()) {
            extra = budget_.try_reserve(charge - reservation.bytes());
            if (!extra) {
                return CandidateError::Full;
            }
        }
        candidates_.push_back(
            {std::move(candidate), std::move(mid), std::move(reservation), std::move(extra)});
        bytes_ += charge;
        return std::monostate{};
    } catch (const std::bad_alloc &) {
        return CandidateError::AllocationFailed;
    } catch (const std::length_error &) {
        return CandidateError::AllocationFailed;
    }
}

bool CandidateBuffer::mark_description_applied() {
    if (closed_ || description_applied_) {
        return false;
    }
    description_applied_ = true;
    return true;
}

std::optional<BufferedCandidate> CandidateBuffer::pop() {
    if (closed_ || !description_applied_ || candidates_.empty()) {
        return std::nullopt;
    }
    auto item = std::move(candidates_.front());
    candidates_.pop_front();
    bytes_ -= item.reservation.bytes() + (item.extra ? item.extra->bytes() : 0);
    return item;
}

void CandidateBuffer::close() {
    closed_ = true;
    candidates_.clear();
    bytes_ = 0;
}
std::size_t CandidateBuffer::size() const { return candidates_.size(); }

} // namespace p2pdc::detail
