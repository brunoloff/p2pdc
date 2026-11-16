#include "relay_candidate_filter.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace p2pdc::detail {
namespace {
std::string redact_related_address(const std::string &candidate) {
    std::istringstream input(candidate);
    std::vector<std::string> tokens;
    for (std::string token; input >> token;)
        tokens.push_back(std::move(token));
    // "candidate:F C T P addr port typ type" is followed by name/value pairs.
    for (std::size_t index = 8; index + 1 < tokens.size(); index += 2) {
        if (tokens[index] == "raddr")
            tokens[index + 1] = "0.0.0.0";
        else if (tokens[index] == "rport")
            tokens[index + 1] = "0";
    }
    std::string output;
    for (const auto &token : tokens)
        output += (output.empty() ? "" : " ") + token;
    return output;
}
} // namespace

std::optional<rtc::Candidate> relay_only_candidate(const rtc::Candidate &candidate) {
    if (candidate.type() != rtc::Candidate::Type::Relayed)
        return std::nullopt;
    rtc::Candidate redacted(redact_related_address(candidate.candidate()), candidate.mid());
    redacted.resolve(rtc::Candidate::ResolveMode::Simple);
    return redacted;
}

std::size_t retain_relay_candidates(rtc::Description &description) {
    auto candidates = description.extractCandidates();
    std::vector<rtc::Candidate> kept;
    for (const auto &candidate : candidates)
        if (auto relay = relay_only_candidate(candidate))
            kept.push_back(std::move(*relay));
    const auto removed = candidates.size() - kept.size();
    description.addCandidates(std::move(kept));
    return removed;
}

} // namespace p2pdc::detail
