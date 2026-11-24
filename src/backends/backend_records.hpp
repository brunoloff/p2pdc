#pragma once

#include "../discovery/record_store.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace p2pdc::detail {

// Why a network backend operation failed, as browser backends tell it (proxy_codec.mjs):
// whether asking again, or elsewhere, may help.
enum class BackendFailure {
    Transient,     // unreachable, timed out, closed, busy or rate limited
    Rejected,      // the server refused; asking again will not change that
    InvalidStream, // the server sent something malformed
};
[[nodiscard]] StoreError store_error(BackendFailure failure) noexcept;

using RecordKey = std::array<std::byte, 20>;

// A value as a backend returns it; id identifies it within its backend.
struct BackendRecord {
    Bytes bytes;
    std::string id;
};

// How long backends that set their own expiry keep a value: as long as OpenDHT does.
inline constexpr std::chrono::seconds record_lifetime{600};
// How long a backend waits for a server's reply.
inline constexpr std::chrono::milliseconds backend_timeout{10000};
// Records a watch admits per second; the rest are dropped so that a hostile server cannot
// flood a node through one watch.
inline constexpr std::size_t max_records_per_second = 256;

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::string to_hex(std::span<const unsigned char> bytes);
// 16 random bytes in hex.
[[nodiscard]] std::string random_id();
[[nodiscard]] std::string encode_base64(ByteView bytes);
// Nullopt unless text is canonical, padded base64 of at most max_bytes bytes.
[[nodiscard]] std::optional<Bytes> decode_base64(std::string_view text, std::size_t max_bytes);
// Nullopt unless key is 20 bytes, not all zero.
[[nodiscard]] std::optional<RecordKey> record_key(ByteView key);

// Gathers a lookup's records within limits, as RecordCollector in store_limits.mjs does:
// records count as the same when both id and content match, or with by_content when the
// content does. Records beyond the limits are dropped and counted.
class RecordCollector {
  public:
    RecordCollector(StoreLimits limits, bool by_content);
    // False once enough records were inspected that the rest is untrusted surplus.
    bool add(const BackendRecord &record);
    [[nodiscard]] StoreBatch take() { return std::exchange(batch_, {}); }

  private:
    StoreLimits limits_;
    bool by_content_;
    std::set<std::pair<std::string, Bytes>> seen_;
    std::size_t total_ = 0;
    std::size_t inspected_ = 0;
    StoreBatch batch_;
};

// Admits at most per_second records in each one-second window.
class RateGate {
  public:
    explicit RateGate(std::size_t per_second) : limit_(per_second) {}
    bool admit();

  private:
    std::size_t limit_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::size_t count_ = 0;
};

} // namespace p2pdc::detail
