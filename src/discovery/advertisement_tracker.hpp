#pragma once

#include "p2pdc/advertisement.h"

#include <map>
#include <optional>
#include <vector>

namespace p2pdc::detail {

enum class AdvertisementEvent {
    Seen,
    Updated,
    Duplicate,
    Stale,
    Conflict,
    Invalid,
    Full,
    ClockRegressed
};
enum class AdvertisementSweepError { ClockRegressed };
using AdvertisementSweep = std::variant<std::vector<Advertisement>, AdvertisementSweepError>;
using AdvertisementCurrent = std::variant<bool, AdvertisementSweepError>;
using AdvertisementSnapshot = std::variant<std::vector<Advertisement>, AdvertisementSweepError>;

// Only records whose signature and capability were verified by the caller may
// be admitted. The tracker also validates fields and times, but it does not
// grant application authorization or prove network reachability. Calls are
// serialized and now must not move backwards. Use one tracker per capability.
// Expired entries retain sequence
// watermarks until no older, previously admissible record can still be valid.
// This finite-retention guarantee assumes each honest instance uses a
// nondecreasing issued_at as its sequence increases; unseen maliciously
// pre-signed future-dated older records cannot be ruled out after eviction.
// Call sweep before observe when expiration events must precede new events.
// Tombstones count toward max_entries; Full requires explicit recovery rather
// than evicting a watermark and silently permitting replay. State is in-memory.
class AdvertisementTracker {
  public:
    AdvertisementTracker(std::size_t max_entries, std::uint64_t max_lifetime,
                         std::uint64_t clock_skew);
    [[nodiscard]] AdvertisementEvent observe(const Advertisement &advertisement, std::uint64_t now);
    [[nodiscard]] AdvertisementSweep sweep(std::uint64_t now);
    // Exact authenticated record, still visible and valid at a nonregressing time.
    [[nodiscard]] AdvertisementCurrent is_current(const Advertisement &value, std::uint64_t now);
    // Does not sweep or emit expiration events; bounded by max_entries.
    [[nodiscard]] AdvertisementSnapshot current_peers(std::uint64_t now);
    [[nodiscard]] std::size_t size() const;

  private:
    struct Entry {
        Advertisement value;
        std::uint64_t retain_until;
        bool visible;
    };
    using Key = std::pair<SigningPublicKey, InstanceId>;
    [[nodiscard]] bool accept_time(std::uint64_t now);
    [[nodiscard]] bool valid(const Advertisement &value, std::uint64_t now) const;
    [[nodiscard]] std::uint64_t retention(std::uint64_t now) const;
    std::size_t max_entries_;
    std::uint64_t max_lifetime_;
    std::uint64_t clock_skew_;
    std::optional<std::uint64_t> last_now_;
    std::map<Key, Entry> entries_;
};

} // namespace p2pdc::detail
