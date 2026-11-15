#include <p2pdc/frame.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <variant>

namespace {
std::uint32_t number(const std::uint8_t *data) {
    return (std::uint32_t{data[0]} << 24U) | (std::uint32_t{data[1]} << 16U) |
           (std::uint32_t{data[2]} << 8U) | std::uint32_t{data[3]};
}
void check(bool condition) {
    if (!condition)
        std::abort();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 8)
        return 0;
    const p2pdc::FrameLimits limits{number(data), number(data + 4)};
    const p2pdc::ByteView wire{reinterpret_cast<const std::byte *>(data + 8), size - 8};
    const auto view = p2pdc::decode_frame_view(wire, limits);
    const auto owned = p2pdc::decode_frame(wire, limits);
    check(view.index() == owned.index());
    if (const auto *error = std::get_if<p2pdc::FrameError>(&view)) {
        check(*error == std::get<p2pdc::FrameError>(owned));
        return 0;
    }
    const auto &borrowed = std::get<p2pdc::FrameView>(view);
    const auto &copy = std::get<p2pdc::Frame>(owned);
    check(borrowed.message_id == copy.message_id && borrowed.total_size == copy.total_size &&
          borrowed.offset == copy.offset &&
          std::equal(borrowed.payload.begin(), borrowed.payload.end(), copy.payload.begin(),
                     copy.payload.end()));
    const auto encoded = p2pdc::encode_frame(copy, limits);
    const auto *bytes = std::get_if<p2pdc::Bytes>(&encoded);
    check(bytes && std::equal(bytes->begin(), bytes->end(), wire.begin(), wire.end()));
    return 0;
}
