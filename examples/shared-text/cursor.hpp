#pragma once

#include "p2pdc/frame.h"

#include <cstdint>
#include <optional>

namespace shared_text {
struct Cursor {
    std::uint32_t position;
    std::uint32_t color;
};
[[nodiscard]] p2pdc::Bytes encodeCursor(Cursor cursor);
[[nodiscard]] std::optional<Cursor> decodeCursor(p2pdc::ByteView message);
} // namespace shared_text
