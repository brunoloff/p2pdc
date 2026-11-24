#pragma once

#include "backend_records.hpp"
#include "schnorr.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace p2pdc::detail {

// Application data (NIP-78); the `x` tag holds the key in hex, which relays index (NIP-01).
inline constexpr int nostr_kind = 30078;

// A signed event carrying value under key, expiring after record_lifetime (NIP-40): the
// same events nostr_relay.mjs publishes.
[[nodiscard]] nlohmann::json nostr_event(const SchnorrSigner &signer, const RecordKey &key,
                                         ByteView value, std::int64_t created_at);
// The record an event carries, or nullopt for an event that is not a current value under
// key_hex: wrong kind or key, expired, or content that is not base64 of at most max_bytes.
[[nodiscard]] std::optional<BackendRecord> nostr_record(const nlohmann::json &event,
                                                        const std::string &key_hex,
                                                        std::size_t max_bytes, std::int64_t now);
// A relay's refusal (NIP-01 machine-readable prefixes): rate limits and relay errors may
// pass; anything else will not change by asking again.
[[nodiscard]] BackendFailure nostr_refusal(const std::string &message);
// Seconds since the Unix epoch.
[[nodiscard]] std::int64_t unix_now();

} // namespace p2pdc::detail
