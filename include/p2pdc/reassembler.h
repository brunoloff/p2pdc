#pragma once

#include "p2pdc/frame.h"

#include <optional>

namespace p2pdc {

struct ReassemblyResult {
    std::optional<Bytes> message;
    std::optional<FrameError> error;
};

// Reliable fragments are ordered and messages must not be interleaved.
class Reassembler {
  public:
    explicit Reassembler(FrameLimits limits);
    [[nodiscard]] ReassemblyResult push(ByteView bytes);
    void reset();
    [[nodiscard]] std::size_t buffered_bytes() const;
    [[nodiscard]] std::size_t allocated_bytes() const;

  private:
    FrameLimits limits_;
    std::optional<std::uint64_t> message_id_;
    std::uint32_t total_size_ = 0;
    Bytes buffer_;
};

} // namespace p2pdc
