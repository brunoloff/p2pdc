#pragma once

#include "p2pdc/frame.h"

#include <optional>

namespace p2pdc {

struct NextFrame {
    std::optional<Bytes> frame;
    std::optional<FrameError> error;
};

// The fragmenter owns its message; a result without frame or error means finished.
class Fragmenter {
  public:
    Fragmenter(Bytes message, std::uint64_t message_id, FrameLimits limits);
    Fragmenter(const Fragmenter &) = delete;
    Fragmenter &operator=(const Fragmenter &) = delete;
    Fragmenter(Fragmenter &&) = delete;
    Fragmenter &operator=(Fragmenter &&) = delete;
    [[nodiscard]] NextFrame next();
    [[nodiscard]] std::size_t allocated_bytes() const;

  private:
    Bytes message_;
    std::uint64_t message_id_;
    FrameLimits limits_;
    std::size_t offset_ = 0;
    bool finished_ = false;
};

} // namespace p2pdc
