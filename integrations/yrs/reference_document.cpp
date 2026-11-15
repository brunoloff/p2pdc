#include "reference_document.hpp"

#include <limits>
#include <stdexcept>

namespace p2pdc_yrs_reference {
namespace {
std::uint32_t checked_length(p2pdc::ByteView bytes) {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Yrs payload is too large");
    return static_cast<std::uint32_t>(bytes.size());
}
p2pdc::Bytes take_binary(char *data, std::uint32_t length) {
    if (!data)
        throw std::runtime_error("Yrs could not encode binary data");
    auto release = [length](char *value) { ybinary_destroy(value, length); };
    std::unique_ptr<char, decltype(release)> owned(data, release);
    auto begin = reinterpret_cast<const std::byte *>(data);
    return p2pdc::Bytes(begin, begin + length);
}
} // namespace
Document::Document() : doc_(ydoc_new(), ydoc_destroy), text_(nullptr) {
    if (!doc_ || !(text_ = ytext(doc_.get(), "text")))
        throw std::runtime_error("Yrs document creation failed");
}
Document::Transaction Document::transaction() const {
    Transaction result(ydoc_write_transaction(doc_.get(), 0, nullptr), ytransaction_commit);
    if (!result)
        throw std::runtime_error("Yrs transaction creation failed");
    return result;
}
void Document::append(const std::string &value) {
    auto txn = transaction();
    ytext_insert(text_, txn.get(), ytext_len(text_, txn.get()), value.c_str(), nullptr);
}
std::string Document::text() const {
    auto txn = transaction();
    std::unique_ptr<char, decltype(&ystring_destroy)> result(ytext_string(text_, txn.get()),
                                                             ystring_destroy);
    if (!result)
        throw std::runtime_error("Yrs could not read text");
    return result.get();
}
p2pdc::Bytes Document::state_vector() const {
    auto txn = transaction();
    std::uint32_t length = 0;
    char *data = ytransaction_state_vector_v1(txn.get(), &length);
    return take_binary(data, length);
}
p2pdc::Bytes Document::difference(p2pdc::ByteView state_vector) const {
    if (state_vector.empty())
        throw std::invalid_argument("Yrs state vector is empty");
    auto txn = transaction();
    std::uint32_t length = 0;
    char *data =
        ytransaction_state_diff_v1(txn.get(), reinterpret_cast<const char *>(state_vector.data()),
                                   checked_length(state_vector), &length);
    return take_binary(data, length);
}
bool Document::apply(p2pdc::ByteView update) {
    if (update.empty())
        return false;
    auto txn = transaction();
    return ytransaction_apply(txn.get(), reinterpret_cast<const char *>(update.data()),
                              checked_length(update)) == 0;
}
} // namespace p2pdc_yrs_reference
