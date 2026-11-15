#pragma once

#include "p2pdc/route.h"

namespace p2pdc_echo {
[[nodiscard]] inline const char *candidate_name(p2pdc::CandidateKind kind) {
    switch (kind) {
    case p2pdc::CandidateKind::Host:
        return "Host";
    case p2pdc::CandidateKind::ServerReflexive:
        return "ServerReflexive";
    case p2pdc::CandidateKind::PeerReflexive:
        return "PeerReflexive";
    case p2pdc::CandidateKind::Relayed:
        return "Relayed";
    case p2pdc::CandidateKind::Unknown:
        return "Unknown";
    }
    return "Unknown";
}
} // namespace p2pdc_echo
