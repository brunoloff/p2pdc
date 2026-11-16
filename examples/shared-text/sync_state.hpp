#pragma once

#include "document.hpp"

#include <utility>

namespace shared_text {
struct PreparedUpdate {
    p2pdc::Bytes data;
    p2pdc::Bytes state;
};

// Tracks what was queued on one ordered connection, not remote persistence.
class PeerSync {
  public:
    [[nodiscard]] PreparedUpdate prepare(const Document &document) const {
        if (queued_.empty()) {
            Document empty;
            return {document.difference(empty.stateVector()), document.stateVector()};
        }
        return {document.difference(queued_), document.stateVector()};
    }
    void accepted(PreparedUpdate update) { queued_ = std::move(update.state); }
    void reset() { queued_.clear(); }

  private:
    p2pdc::Bytes queued_;
};
} // namespace shared_text
