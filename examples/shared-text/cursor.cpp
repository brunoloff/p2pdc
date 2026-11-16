#include "cursor.hpp"

#include <stdexcept>

namespace shared_text {
using namespace p2pdc;
Bytes encodeCursor(Cursor cursor) {
    if (cursor.position > 65536 || cursor.color > 0xffffff)
        throw std::invalid_argument("invalid cursor position or color");
    Bytes bytes{std::byte{2}};
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::byte>((cursor.position >> shift) & 0xff));
    for (int shift = 16; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::byte>((cursor.color >> shift) & 0xff));
    return bytes;
}
std::optional<Cursor> decodeCursor(ByteView message) {
    if (message.size() != 8 || message[0] != std::byte{2})
        return std::nullopt;
    Cursor cursor{0, 0};
    for (std::size_t i = 1; i < 5; ++i)
        cursor.position = (cursor.position << 8) | std::to_integer<unsigned>(message[i]);
    for (std::size_t i = 5; i < 8; ++i)
        cursor.color = (cursor.color << 8) | std::to_integer<unsigned>(message[i]);
    if (cursor.position > 65536)
        return std::nullopt;
    return cursor;
}
} // namespace shared_text
