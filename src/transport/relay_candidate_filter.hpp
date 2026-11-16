#pragma once

#include <rtc/description.hpp>

#include <cstddef>
#include <optional>

namespace p2pdc::detail {

// RequireLocalRelay must not disclose host or server-reflexive addresses to the
// peer, independently of whether the ICE backend honours force-relay. These
// helpers enforce that on what is signaled: only relayed local candidates are
// kept, and their related address (libnice reports the local host socket
// there) is redacted to 0.0.0.0:0 as browsers do. Removing host candidates also
// clears the SDP connection address, which libdatachannel derives from the
// best host candidate.
[[nodiscard]] std::optional<rtc::Candidate> relay_only_candidate(const rtc::Candidate &candidate);
// Returns the number of candidates removed from the description.
std::size_t retain_relay_candidates(rtc::Description &description);

} // namespace p2pdc::detail
