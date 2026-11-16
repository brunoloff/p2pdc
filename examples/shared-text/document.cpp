#include "document.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace shared_text {
namespace {
std::uint32_t length(p2pdc::ByteView bytes) {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Yrs data exceeds the supported size");
    return static_cast<std::uint32_t>(bytes.size());
}
p2pdc::Bytes takeBinary(char *data, std::uint32_t size) {
    if (!data)
        throw std::runtime_error("Yrs could not encode an update");
    auto deleter = [size](char *value) { ybinary_destroy(value, size); };
    std::unique_ptr<char, decltype(deleter)> owned(data, deleter);
    const auto *start = reinterpret_cast<const std::byte *>(data);
    return {start, start + size};
}
qsizetype prefixLength(const QString &old, const QString &value) {
    qsizetype n = 0;
    while (n < old.size() && n < value.size() && old[n] == value[n])
        ++n;
    if (n && n < old.size() && old[n].isLowSurrogate() && old[n - 1].isHighSurrogate())
        --n;
    return n;
}
qsizetype suffixLength(const QString &old, const QString &value, qsizetype prefix) {
    qsizetype n = 0;
    while (n < old.size() - prefix && n < value.size() - prefix &&
           old[old.size() - n - 1] == value[value.size() - n - 1])
        ++n;
    if (n && old[old.size() - n].isLowSurrogate() && old.size() > n &&
        old[old.size() - n - 1].isHighSurrogate())
        --n;
    return n;
}
} // namespace
Document::Document() : doc_(nullptr, ydoc_destroy) {
    auto options = yoptions();
    options.flags = static_cast<std::uint8_t>((options.flags & ~Y_OFFSET_UTF16) | Y_OFFSET_UTF16);
    doc_.reset(ydoc_new_with_options(options));
    if (!doc_ || !(text_ = ytext(doc_.get(), "text")))
        throw std::runtime_error("Yrs document creation failed");
}
Document::Transaction Document::transaction() const {
    Transaction result(ydoc_write_transaction(doc_.get(), 0, nullptr), ytransaction_commit);
    if (!result)
        throw std::runtime_error("Yrs transaction creation failed");
    return result;
}
QString Document::text() const {
    auto txn = transaction();
    std::unique_ptr<char, decltype(&ystring_destroy)> result(ytext_string(text_, txn.get()),
                                                             ystring_destroy);
    if (!result)
        throw std::runtime_error("Yrs could not read the document");
    return QString::fromUtf8(result.get());
}
p2pdc::Bytes Document::stateVector() const {
    auto txn = transaction();
    std::uint32_t size = 0;
    char *data = ytransaction_state_vector_v1(txn.get(), &size);
    return takeBinary(data, size);
}
p2pdc::Bytes Document::difference(p2pdc::ByteView vector) const {
    if (vector.empty())
        throw std::invalid_argument("empty Yrs state vector");
    auto txn = transaction();
    std::uint32_t size = 0;
    char *data = ytransaction_state_diff_v1(
        txn.get(), reinterpret_cast<const char *>(vector.data()), length(vector), &size);
    return takeBinary(data, size);
}
bool Document::apply(p2pdc::ByteView update) {
    if (update.empty())
        return false;
    auto txn = transaction();
    return ytransaction_apply(txn.get(), reinterpret_cast<const char *>(update.data()),
                              length(update)) == 0;
}
bool Document::replace(const QString &value) {
    if (value.contains(QChar::Null) || value.size() > 65536 || !value.isValidUtf16())
        return false;
    const auto old = text();
    const auto prefix = prefixLength(old, value), suffix = suffixLength(old, value, prefix);
    auto txn = transaction();
    const auto removed = old.size() - prefix - suffix;
    if (removed)
        ytext_remove_range(text_, txn.get(), static_cast<std::uint32_t>(prefix),
                           static_cast<std::uint32_t>(removed));
    const auto inserted = value.mid(prefix, value.size() - prefix - suffix).toUtf8();
    if (!inserted.isEmpty())
        ytext_insert(text_, txn.get(), static_cast<std::uint32_t>(prefix), inserted.constData(),
                     nullptr);
    return true;
}
} // namespace shared_text
