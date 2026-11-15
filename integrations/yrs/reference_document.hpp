#pragma once

#include <p2pdc/frame.h>

extern "C" {
#include <libyrs.h>
}

#include <memory>
#include <string>

namespace p2pdc_yrs_reference {
class Document {
  public:
    Document();
    void append(const std::string &text);
    [[nodiscard]] std::string text() const;
    [[nodiscard]] p2pdc::Bytes state_vector() const;
    [[nodiscard]] p2pdc::Bytes difference(p2pdc::ByteView state_vector) const;
    [[nodiscard]] bool apply(p2pdc::ByteView update);

  private:
    using Transaction = std::unique_ptr<YTransaction, decltype(&ytransaction_commit)>;
    [[nodiscard]] Transaction transaction() const;
    std::unique_ptr<YDoc, decltype(&ydoc_destroy)> doc_;
    Branch *text_;
};
} // namespace p2pdc_yrs_reference
