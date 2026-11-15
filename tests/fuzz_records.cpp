#include "../src/security/signaling_candidate.hpp"
#include "p2pdc/advertisement.h"
#include "p2pdc/handshake.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <variant>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Fixture {
    Identity offerer = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity answerer = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    Identity recipient_id =
        std::get<Identity>(Identity::import_seed(std::array<std::byte, 32>{std::byte{8}}));
    RecipientKeyPair box = std::get<RecipientKeyPair>(
        RecipientKeyPair::import_secret_key(std::array<std::byte, 32>{std::byte{17}}));
    HandshakeContext context{offerer.public_key(), answerer.public_key(), {std::byte{1}},
                             {std::byte{2}},       {std::byte{3}},        {std::byte{4}},
                             {std::byte{5}},       {std::byte{6}}};
};

void candidate(ByteView wire) {
    const CandidateLimits limits{4096, 256, 4361};
    const auto decoded = decode_candidate(wire, limits);
    if (const auto *value = std::get_if<CandidateView>(&decoded)) {
        auto encoded = encode_candidate(*value, limits);
        const auto *bytes = std::get_if<Bytes>(&encoded);
        if (!bytes || !std::equal(bytes->begin(), bytes->end(), wire.begin(), wire.end()))
            std::abort();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (!size)
        return 0;
    static Fixture fixture;
    const ByteView wire{reinterpret_cast<const std::byte *>(data + 1), size - 1};
    switch (data[0] % 4) {
    case 0:
        (void)decode_advertisement(wire, std::array<std::byte, 32>{std::byte{7}}, {100, 60, 5});
        break;
    case 1:
        (void)decode_signaling(wire, fixture.box,
                               {fixture.recipient_id.public_key(), {std::byte{2}}},
                               std::array<std::byte, 32>{std::byte{9}}, {100, 60, 5, 4096});
        break;
    case 2:
        candidate(wire);
        break;
    case 3:
        (void)decode_handshake(wire, fixture.context, HandshakeRole::Offerer);
        break;
    }
    return 0;
}
