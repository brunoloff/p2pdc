#include "nostr_event.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <chrono>

namespace p2pdc::detail {
namespace {

std::optional<std::string> tag(const nlohmann::json &event, const char *name) {
    const auto tags = event.find("tags");
    if (tags == event.end() || !tags->is_array())
        return std::nullopt;
    for (const auto &entry : *tags) {
        if (entry.is_array() && entry.size() >= 2 && entry[0] == name && entry[1].is_string())
            return entry[1].get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::int64_t> number(const std::string &text) {
    try {
        std::size_t used = 0;
        const auto value = std::stoll(text, &used);
        return used == text.size() ? std::optional(value) : std::nullopt;
    } catch (const std::logic_error &) {
        return std::nullopt;
    }
}

} // namespace

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

nlohmann::json nostr_event(const SchnorrSigner &signer, const RecordKey &key, ByteView value,
                           std::int64_t created_at) {
    const auto pubkey = to_hex(std::span<const unsigned char>(signer.public_key()));
    const auto tags = nlohmann::json::array(
        {{"d", random_id()},
         {"x", to_hex(key)},
         {"expiration", std::to_string(created_at + record_lifetime.count())}});
    const auto content = encode_base64(value);
    const auto serial =
        nlohmann::json::array({0, pubkey, created_at, nostr_kind, tags, content}).dump();
    std::array<unsigned char, crypto_hash_sha256_BYTES> id{};
    crypto_hash_sha256(id.data(), reinterpret_cast<const unsigned char *>(serial.data()),
                       serial.size());
    const auto signature = signer.sign(id);
    return {{"id", to_hex(std::span<const unsigned char>(id))},
            {"pubkey", pubkey},
            {"created_at", created_at},
            {"kind", nostr_kind},
            {"tags", tags},
            {"content", content},
            {"sig", to_hex(std::span<const unsigned char>(signature))}};
}

std::optional<BackendRecord> nostr_record(const nlohmann::json &event, const std::string &key_hex,
                                          std::size_t max_bytes, std::int64_t now) {
    if (!event.is_object() || !event.contains("kind") || !event["kind"].is_number_integer() ||
        event["kind"] != nostr_kind || tag(event, "x") != key_hex || !event.contains("id") ||
        !event["id"].is_string() || !event.contains("content") || !event["content"].is_string())
        return std::nullopt;
    if (const auto expiration = tag(event, "expiration")) {
        const auto expires = number(*expiration);
        if (expires && *expires <= now)
            return std::nullopt;
    }
    auto bytes = decode_base64(event["content"].get_ref<const std::string &>(), max_bytes);
    if (!bytes)
        return std::nullopt;
    return BackendRecord{std::move(*bytes), event["id"].get<std::string>()};
}

BackendFailure nostr_refusal(const std::string &message) {
    const auto prefix = message.substr(0, message.find(':'));
    return prefix == "rate-limited" || prefix == "error" ? BackendFailure::Transient
                                                         : BackendFailure::Rejected;
}

} // namespace p2pdc::detail
