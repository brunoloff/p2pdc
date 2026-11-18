#pragma once

#include <string>
#include <string_view>

namespace p2pdc::detail {

// Datagrams travel on a second, unordered data channel without retransmission. The
// channel shares the connection's DTLS session, whose fingerprints the signed offer
// and answer bind to the peers' identities, so it needs no handshake of its own.
//
// Earlier versions fail a connection on seeing an unknown data channel, so each side
// announces support with a line in its signed description, and the offerer opens the
// channel only after the answer carries it. Other implementations ignore the line, but
// libdatachannel copies unknown attributes of an offer into its answer; the two roles
// therefore use different lines, so an echoed offer line does not count as support.
inline constexpr std::string_view datagram_channel_label = "p2pdc/datagram/v1";
inline constexpr std::string_view datagram_offer_attribute = "a=x-p2pdc-datagrams:offer";
inline constexpr std::string_view datagram_answer_attribute = "a=x-p2pdc-datagrams:answer";

[[nodiscard]] inline std::string advertise_datagrams(std::string sdp, std::string_view attribute) {
    if (!sdp.empty() && !sdp.ends_with('\n'))
        sdp += "\r\n";
    sdp += attribute;
    sdp += "\r\n";
    return sdp;
}

[[nodiscard]] inline bool advertises_datagrams(std::string_view sdp, std::string_view attribute) {
    std::size_t start = 0;
    while (start < sdp.size()) {
        const auto end = sdp.find('\n', start);
        auto line = sdp.substr(start, end == std::string_view::npos ? end : end - start);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line == attribute)
            return true;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return false;
}

} // namespace p2pdc::detail
