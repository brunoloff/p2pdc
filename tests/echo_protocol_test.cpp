#include "../examples/echo/echo_protocol.hpp"
#include "../examples/echo/route_display.hpp"

#include <gtest/gtest.h>

using namespace p2pdc_echo;

TEST(EchoProtocolTest, ConfirmsEchoAndRejectsDuplicates) {
    EchoProtocol initiator(Role::Initiator), responder(Role::Responder);
    EXPECT_EQ(responder.receive(echo_message), EchoAction::SendEcho);
    EXPECT_EQ(initiator.receive(echo_message), EchoAction::SendAck);
    EXPECT_EQ(responder.receive(echo_ack), EchoAction::SendConfirmation);
    EXPECT_EQ(initiator.receive(echo_confirmation), EchoAction::Complete);
    EXPECT_EQ(responder.receive(echo_ack), EchoAction::Invalid);
    EXPECT_EQ(initiator.receive(echo_confirmation), EchoAction::Invalid);
}

TEST(EchoProtocolTest, RejectsUnexpectedOrOutOfOrderMessages) {
    EchoProtocol initiator(Role::Initiator), responder(Role::Responder);
    EXPECT_EQ(initiator.receive(echo_confirmation), EchoAction::Invalid);
    EXPECT_EQ(responder.receive(echo_ack), EchoAction::Invalid);
    const std::array bad{std::byte{7}};
    EXPECT_EQ(responder.receive(bad), EchoAction::Invalid);
    EXPECT_EQ(responder.receive(echo_message), EchoAction::SendEcho);
    EXPECT_EQ(responder.receive(echo_confirmation), EchoAction::Invalid);
}

TEST(EchoRouteDisplayTest, NamesKnownAndUnknownCandidates) {
    EXPECT_STREQ(candidate_name(p2pdc::CandidateKind::PeerReflexive), "PeerReflexive");
    EXPECT_STREQ(candidate_name(p2pdc::CandidateKind::Relayed), "Relayed");
    EXPECT_STREQ(candidate_name(p2pdc::CandidateKind::Unknown), "Unknown");
}
