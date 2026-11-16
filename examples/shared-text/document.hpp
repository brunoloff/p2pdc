#pragma once

#include <QString>
#include <memory>
#include <p2pdc/frame.h>

extern "C" {
#include <libyrs.h>
}

namespace shared_text {
class Document {
  public:
    Document();
    [[nodiscard]] QString text() const;
    [[nodiscard]] p2pdc::Bytes stateVector() const;
    [[nodiscard]] p2pdc::Bytes difference(p2pdc::ByteView vector) const;
    [[nodiscard]] bool apply(p2pdc::ByteView update);
    [[nodiscard]] bool replace(const QString &value);

  private:
    using Transaction = std::unique_ptr<YTransaction, decltype(&ytransaction_commit)>;
    [[nodiscard]] Transaction transaction() const;
    std::unique_ptr<YDoc, decltype(&ydoc_destroy)> doc_;
    Branch *text_ = nullptr;
};
} // namespace shared_text
