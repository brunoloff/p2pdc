#include "../src/transport/datagram_support.hpp"

#include <gtest/gtest.h>

namespace p2pdc::detail {
namespace {

TEST(DatagramSupport, AnnouncedDescriptionsAreRecognized) {
    const std::string sdp = "v=0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";
    EXPECT_FALSE(advertises_datagrams(sdp, datagram_offer_attribute));
    const std::string announced = advertise_datagrams(sdp, datagram_offer_attribute);
    EXPECT_TRUE(announced.starts_with(sdp));
    EXPECT_TRUE(announced.ends_with("\r\n"));
    EXPECT_TRUE(advertises_datagrams(announced, datagram_offer_attribute));
}

// libdatachannel copies unknown offer attributes into its answers, including those of
// versions that know nothing about datagrams.
TEST(DatagramSupport, AnEchoedOfferLineIsNotAnAnswer) {
    const std::string echoed = advertise_datagrams("v=0\r\n", datagram_offer_attribute);
    EXPECT_FALSE(advertises_datagrams(echoed, datagram_answer_attribute));
}

TEST(DatagramSupport, AnnouncingCompletesAnUnfinishedLastLine) {
    EXPECT_EQ(advertise_datagrams("v=0", datagram_answer_attribute),
              "v=0\r\na=x-p2pdc-datagrams:answer\r\n");
}

TEST(DatagramSupport, OnlyTheWholeLineCounts) {
    EXPECT_FALSE(advertises_datagrams("a=x-p2pdc-datagrams:offers\r\n", datagram_offer_attribute));
    EXPECT_FALSE(advertises_datagrams("xa=x-p2pdc-datagrams:offer\r\n", datagram_offer_attribute));
    EXPECT_TRUE(advertises_datagrams("v=0\na=x-p2pdc-datagrams:offer", datagram_offer_attribute));
    EXPECT_FALSE(advertises_datagrams("", datagram_offer_attribute));
}

} // namespace
} // namespace p2pdc::detail
