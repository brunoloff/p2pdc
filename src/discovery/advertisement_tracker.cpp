#include "advertisement_tracker.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace p2pdc::detail {
namespace {
bool nonzero(ByteView bytes) {
    return std::any_of(bytes.begin(), bytes.end(),
                       [](std::byte value) { return value != std::byte{}; });
}
bool expired(std::uint64_t now, std::uint64_t expiry, std::uint64_t skew) {
    return now >= expiry && now - expiry >= skew;
}
std::uint64_t add_saturating(std::uint64_t a, std::uint64_t b) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return b > maximum - a ? maximum : a + b;
}
} // namespace

AdvertisementTracker::AdvertisementTracker(std::size_t max_entries, std::uint64_t max_lifetime,
                                           std::uint64_t clock_skew)
    : max_entries_(max_entries), max_lifetime_(max_lifetime), clock_skew_(clock_skew) {
    if (max_entries == 0 || max_lifetime == 0)
        throw std::invalid_argument("invalid advertisement tracker limits");
}

bool AdvertisementTracker::accept_time(std::uint64_t now) {
    if (last_now_ && now < *last_now_)
        return false;
    last_now_ = now;
    return true;
}
bool AdvertisementTracker::valid(const Advertisement &value, std::uint64_t now) const {
    const auto &fields = value.fields;
    return nonzero(value.signing_public_key) && nonzero(fields.recipient_public_key) &&
           nonzero(fields.instance_id) && nonzero(fields.signaling_inbox) &&
           fields.expires_at > fields.issued_at &&
           fields.expires_at - fields.issued_at <= max_lifetime_ &&
           (fields.issued_at <= now || fields.issued_at - now <= clock_skew_) &&
           !expired(now, fields.expires_at, clock_skew_);
}
std::uint64_t AdvertisementTracker::retention(std::uint64_t now) const {
    return add_saturating(add_saturating(add_saturating(now, max_lifetime_), clock_skew_),
                          clock_skew_);
}

AdvertisementEvent AdvertisementTracker::observe(const Advertisement &value, std::uint64_t now) {
    if (!accept_time(now))
        return AdvertisementEvent::ClockRegressed;
    if (!valid(value, now))
        return AdvertisementEvent::Invalid;
    const Key key{value.signing_public_key, value.fields.instance_id};
    auto found = entries_.find(key);
    if (found == entries_.end()) {
        if (entries_.size() >= max_entries_)
            return AdvertisementEvent::Full;
        entries_.emplace(key, Entry{value, retention(now), true});
        return AdvertisementEvent::Seen;
    }
    auto &entry = found->second;
    if (value.fields.sequence < entry.value.fields.sequence)
        return AdvertisementEvent::Stale;
    if (value.fields.sequence == entry.value.fields.sequence) {
        return value == entry.value ? AdvertisementEvent::Duplicate : AdvertisementEvent::Conflict;
    }
    if (value.fields.issued_at < entry.value.fields.issued_at)
        return AdvertisementEvent::Invalid;
    const bool was_visible =
        entry.visible && !expired(now, entry.value.fields.expires_at, clock_skew_);
    entry = Entry{value, retention(now), true};
    return was_visible ? AdvertisementEvent::Updated : AdvertisementEvent::Seen;
}

AdvertisementSweep AdvertisementTracker::sweep(std::uint64_t now) {
    if (!accept_time(now))
        return AdvertisementSweepError::ClockRegressed;
    std::vector<Advertisement> expired_entries;
    const auto count = std::count_if(entries_.begin(), entries_.end(), [&](const auto &item) {
        return item.second.visible &&
               expired(now, item.second.value.fields.expires_at, clock_skew_);
    });
    expired_entries.reserve(static_cast<std::size_t>(count));
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto &entry = it->second;
        if (entry.visible && expired(now, entry.value.fields.expires_at, clock_skew_)) {
            expired_entries.push_back(entry.value);
            entry.visible = false;
        }
        if (!entry.visible && now > entry.retain_until)
            it = entries_.erase(it);
        else
            ++it;
    }
    return expired_entries;
}

AdvertisementCurrent AdvertisementTracker::is_current(const Advertisement &value,
                                                      std::uint64_t now) {
    if (!accept_time(now)) {
        return AdvertisementSweepError::ClockRegressed;
    }
    if (!valid(value, now)) {
        return false;
    }
    const Key key{value.signing_public_key, value.fields.instance_id};
    const auto found = entries_.find(key);
    return found != entries_.end() && found->second.visible && found->second.value == value;
}
AdvertisementSnapshot AdvertisementTracker::current_peers(std::uint64_t now) {
    if (!accept_time(now)) {
        return AdvertisementSweepError::ClockRegressed;
    }
    std::vector<Advertisement> result;
    result.reserve(entries_.size());
    for (const auto &[key, entry] : entries_) {
        if (entry.visible && valid(entry.value, now)) {
            result.push_back(entry.value);
        }
    }
    return result;
}
std::size_t AdvertisementTracker::size() const { return entries_.size(); }

} // namespace p2pdc::detail
