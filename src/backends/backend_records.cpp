#include "backend_records.hpp"

#include <sodium.h>

#include <algorithm>

namespace p2pdc::detail {

StoreError store_error(BackendFailure failure) noexcept {
    switch (failure) {
    case BackendFailure::Rejected:
        return StoreError::PermanentFailure;
    case BackendFailure::InvalidStream:
        return StoreError::InvalidRecord;
    case BackendFailure::Transient:
        break;
    }
    return StoreError::NotRunning;
}

std::string to_hex(std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 15]);
    }
    return text;
}

std::string to_hex(std::span<const std::byte> bytes) {
    return to_hex(std::span(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()));
}

std::string random_id() {
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    return to_hex(std::span<const unsigned char>(bytes));
}

std::string encode_base64(ByteView bytes) {
    std::string text(sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(text.data(), text.size(),
                      reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    text.pop_back(); // the terminating zero
    return text;
}

std::optional<Bytes> decode_base64(std::string_view text, std::size_t max_bytes) {
    if (text.size() % 4 != 0 || text.size() / 4 * 3 > max_bytes + 2)
        return std::nullopt;
    Bytes bytes(text.size() / 4 * 3);
    std::size_t length = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size(),
                          text.data(), text.size(), nullptr, &length, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        length > max_bytes)
        return std::nullopt;
    bytes.resize(length);
    if (encode_base64(bytes) != text) // noncanonical padding bits or missing padding
        return std::nullopt;
    return bytes;
}

std::optional<RecordKey> record_key(ByteView key) {
    if (key.size() != 20 || std::ranges::all_of(key, [](std::byte b) { return b == std::byte{}; }))
        return std::nullopt;
    RecordKey result{};
    std::ranges::copy(key, result.begin());
    return result;
}

RecordCollector::RecordCollector(StoreLimits limits, bool by_content)
    : limits_(limits), by_content_(by_content) {}

bool RecordCollector::add(const BackendRecord &record) {
    if (++inspected_ > limits_.max_values * 8)
        return false;
    if (record.bytes.size() > limits_.max_value_bytes) {
        ++batch_.dropped;
        return true;
    }
    auto identity = std::pair(by_content_ ? std::string() : record.id, record.bytes);
    const bool identified = by_content_ || !record.id.empty();
    if (identified && seen_.contains(identity))
        return true;
    if (batch_.values.size() >= limits_.max_values ||
        record.bytes.size() > limits_.max_total_bytes - total_) {
        ++batch_.dropped;
        return true;
    }
    if (identified)
        seen_.insert(std::move(identity));
    total_ += record.bytes.size();
    batch_.values.push_back(record.bytes);
    return true;
}

bool RateGate::admit() {
    const auto now = std::chrono::steady_clock::now();
    if (now - start_ >= std::chrono::seconds(1)) {
        start_ = now;
        count_ = 0;
    }
    return ++count_ <= limit_;
}

} // namespace p2pdc::detail
