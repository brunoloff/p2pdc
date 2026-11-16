#include "../src/transport/relay_candidate_filter.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace p2pdc::detail;

namespace {
const std::string host = "a=candidate:1 1 UDP 2122317823 192.168.1.20 50000 typ host";
const std::string reflexive =
    "a=candidate:2 1 UDP 1686109951 203.0.113.7 50001 typ srflx raddr 192.168.1.20 rport 50000";
const std::string relay =
    "a=candidate:3 1 UDP 41885695 198.51.100.9 3478 typ relay raddr 203.0.113.7 rport 50001";

rtc::Description offer() {
    const std::string sdp = "v=0\r\n"
                            "o=- 1 1 IN IP4 127.0.0.1\r\n"
                            "s=-\r\n"
                            "t=0 0\r\n"
                            "a=group:BUNDLE 0\r\n"
                            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                            "c=IN IP4 0.0.0.0\r\n"
                            "a=mid:0\r\n"
                            "a=sctp-port:5000\r\n"
                            "a=setup:actpass\r\n"
                            "a=ice-ufrag:abcd\r\n"
                            "a=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
                            "a=fingerprint:sha-256 "
                            "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
                            "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n" +
                            host + "\r\n" + reflexive + "\r\n" + relay + "\r\n";
    return rtc::Description(sdp, rtc::Description::Type::Offer);
}
} // namespace

TEST(RelayCandidateFilter, KeepsOnlyRelayedCandidatesWithRedactedRelatedAddress) {
    EXPECT_FALSE(relay_only_candidate(rtc::Candidate(host.substr(2), "0")));
    EXPECT_FALSE(relay_only_candidate(rtc::Candidate(reflexive.substr(2), "0")));
    const auto kept = relay_only_candidate(rtc::Candidate(relay.substr(2), "0"));
    ASSERT_TRUE(kept);
    const auto text = kept->candidate();
    EXPECT_NE(text.find("198.51.100.9 3478 typ relay"), std::string::npos);
    EXPECT_NE(text.find("raddr 0.0.0.0 rport 0"), std::string::npos);
    EXPECT_EQ(text.find("203.0.113.7"), std::string::npos);
    EXPECT_EQ(kept->mid(), "0");
    EXPECT_EQ(kept->type(), rtc::Candidate::Type::Relayed);
}

TEST(RelayCandidateFilter, StripsNonRelayCandidatesAndHostConnectionAddress) {
    auto description = offer();
    ASSERT_NE(std::string(description).find("192.168.1.20 50000 typ host"), std::string::npos);
    EXPECT_EQ(retain_relay_candidates(description), 2U);
    const auto sdp = std::string(description);
    EXPECT_EQ(sdp.find("typ host"), std::string::npos);
    EXPECT_EQ(sdp.find("typ srflx"), std::string::npos);
    EXPECT_NE(sdp.find("typ relay"), std::string::npos);
    EXPECT_EQ(sdp.find("c=IN IP4 192.168.1.20"), std::string::npos);
    EXPECT_NE(sdp.find("a=fingerprint:sha-256"), std::string::npos);
    EXPECT_EQ(sdp.find("192.168.1.20"), std::string::npos);
    EXPECT_EQ(sdp.find("203.0.113.7"), std::string::npos);
}

TEST(RelayCandidateFilter, LeavesADescriptionWithoutCandidatesUnchanged) {
    auto description = offer();
    (void)retain_relay_candidates(description);
    const auto once = std::string(description);
    EXPECT_EQ(retain_relay_candidates(description), 0U);
    EXPECT_EQ(std::string(description), once);
}
