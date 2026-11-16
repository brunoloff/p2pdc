#pragma once

#include "incoming_offer_resolver.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace p2pdc::detail {

inline bool matches_attempt(const SignalingEnvelope &signal, const SignalingEnvelope &expected) {
    return signal.sender_signing_key == expected.sender_signing_key &&
           signal.fields.sender_instance == expected.fields.sender_instance &&
           signal.fields.attempt_id == expected.fields.attempt_id;
}

// Owner-executor only. IDs are never reused during this collection's lifetime.
class PendingOffers {
  public:
    using Id = std::uint64_t;
    using Clock = std::chrono::steady_clock;
    struct Entry {
        IncomingOffer offer;
        Clock::time_point deadline;
    };

    PendingOffers(std::size_t max_count, std::size_t max_bytes)
        : max_count_(max_count), max_bytes_(max_bytes) {
        if (!max_count || !max_bytes)
            throw std::invalid_argument("pending offer limits must be positive");
    }

    // On capacity failure the caller's offer is left intact.
    [[nodiscard]] std::optional<Id> add(IncomingOffer &&offer, Clock::time_point deadline) {
        const auto wire = offer.wire.capacity();
        const auto payload = offer.envelope.fields.payload.capacity();
        if (entries_.size() >= max_count_ || wire > max_bytes_ - bytes_ ||
            payload > max_bytes_ - bytes_ - wire || next_ == std::numeric_limits<Id>::max())
            return std::nullopt;
        const auto id = next_++;
        entries_.emplace(id, Entry{std::move(offer), deadline});
        bytes_ += wire + payload;
        return id;
    }

    [[nodiscard]] const Entry *find(Id id) const {
        auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::optional<Entry> take(Id id) {
        auto it = entries_.find(id);
        if (it == entries_.end())
            return std::nullopt;
        bytes_ -=
            it->second.offer.wire.capacity() + it->second.offer.envelope.fields.payload.capacity();
        auto value = std::move(it->second);
        entries_.erase(it);
        return value;
    }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] std::size_t bytes() const { return bytes_; }
    [[nodiscard]] std::optional<Id> first() const {
        return entries_.empty() ? std::nullopt : std::optional<Id>(entries_.begin()->first);
    }
    [[nodiscard]] std::optional<Id> first_expired(Clock::time_point now) const {
        for (const auto &[id, entry] : entries_)
            if (entry.deadline <= now)
                return id;
        return std::nullopt;
    }
    // Call only after authenticating the signal and checking its kind and sequence.
    [[nodiscard]] std::optional<Id> find_attempt(const SignalingEnvelope &signal) const {
        for (const auto &[id, entry] : entries_) {
            const auto &expected = entry.offer.envelope;
            if (matches_attempt(signal, expected))
                return id;
        }
        return std::nullopt;
    }

  private:
    std::size_t max_count_, max_bytes_, bytes_ = 0;
    Id next_ = 1;
    std::map<Id, Entry> entries_;
};
} // namespace p2pdc::detail
