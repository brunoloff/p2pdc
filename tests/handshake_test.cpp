#include "p2pdc/handshake_exchange.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <stdexcept>
#include <string_view>

using namespace p2pdc;

namespace {
struct Fixture {
    Identity offerer = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity answerer = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    HandshakeContext context() const {
        return {offerer.public_key(), answerer.public_key(), {std::byte{1}}, {std::byte{2}},
                {std::byte{3}},       {std::byte{4}},        {std::byte{5}}, {std::byte{6}}};
    }
    HandshakeRecord record() const {
        return {HandshakeKind::Hello, HandshakeRole::Offerer, 100, {std::byte{7}}, {}};
    }
};
} // namespace

TEST(Handshake, MatchesPublishedHelloVector) {
    Fixture f;
    constexpr std::string_view vector =
#include "handshake_vector.inc"
        ;
    Bytes expected(vector.size() / 2);
    ASSERT_EQ(sodium_hex2bin(reinterpret_cast<unsigned char *>(expected.data()), expected.size(),
                             vector.data(), vector.size(), nullptr, nullptr, nullptr),
              0);
    ASSERT_EQ(expected.size(), handshake_record_size);
    const auto encoded = encode_handshake(f.offerer, f.context(), f.record());
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    EXPECT_EQ(std::get<Bytes>(encoded), expected);
    EXPECT_TRUE(std::holds_alternative<HandshakeRecord>(
        decode_handshake(expected, f.context(), HandshakeRole::Offerer)));
}

TEST(Handshake, SignedRecordBindsRolesContextAndContents) {
    Fixture f;
    const auto wire = std::get<Bytes>(encode_handshake(f.offerer, f.context(), f.record()));
    ASSERT_EQ(wire.size(), handshake_record_size);
    const auto decoded = decode_handshake(wire, f.context(), HandshakeRole::Offerer);
    ASSERT_TRUE(std::holds_alternative<HandshakeRecord>(decoded));
    EXPECT_EQ(std::get<HandshakeRecord>(decoded).max_message_size, 100U);
    EXPECT_EQ(
        std::get<HandshakeError>(decode_handshake(wire, f.context(), HandshakeRole::Answerer)),
        HandshakeError::WrongRole);
    auto changed = f.context();
    changed.attempt[0] = std::byte{9};
    EXPECT_EQ(std::get<CryptoError>(decode_handshake(wire, changed, HandshakeRole::Offerer)),
              CryptoError::AuthenticationFailed);
    changed = f.context();
    changed.offer_hash[0] = std::byte{9};
    EXPECT_EQ(std::get<CryptoError>(decode_handshake(wire, changed, HandshakeRole::Offerer)),
              CryptoError::AuthenticationFailed);
    auto tampered = wire;
    tampered[11] ^= std::byte{1};
    EXPECT_EQ(
        std::get<CryptoError>(decode_handshake(tampered, f.context(), HandshakeRole::Offerer)),
        CryptoError::AuthenticationFailed);
    EXPECT_EQ(std::get<HandshakeError>(encode_handshake(f.answerer, f.context(), f.record())),
              HandshakeError::WrongIdentity);
    EXPECT_EQ(std::get<HandshakeError>(decode_handshake({}, f.context(), HandshakeRole::Offerer)),
              HandshakeError::InvalidRecord);
}

TEST(Handshake, ContextHashesAndNoncesValidateInputs) {
    EXPECT_EQ(std::get<CryptoError>(handshake_group({})), CryptoError::InvalidLength);
    const Bytes capability(32, std::byte{3});
    EXPECT_TRUE(std::holds_alternative<HandshakeHash>(handshake_group(capability)));
    EXPECT_EQ(std::get<CryptoError>(handshake_description_hash({}, 100)),
              CryptoError::InvalidLength);
    const Bytes sdp{std::byte{1}, std::byte{2}};
    EXPECT_EQ(std::get<CryptoError>(handshake_description_hash(sdp, 1)),
              CryptoError::InvalidLength);
    EXPECT_TRUE(std::holds_alternative<HandshakeHash>(handshake_description_hash(sdp, 2)));
    EXPECT_TRUE(std::holds_alternative<HandshakeNonce>(generate_handshake_nonce()));
}

TEST(Handshake, TwoSidedChallengeAndConfirmationNegotiatesMinimum) {
    Fixture f;
    HandshakeExchange a(f.offerer, f.context(), HandshakeRole::Offerer, {std::byte{7}}, 100);
    HandshakeExchange b(f.answerer, f.context(), HandshakeRole::Answerer, {std::byte{8}}, 80);
    const auto hello_a = std::get<Bytes>(a.hello());
    const auto hello_b = std::get<Bytes>(b.hello());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.mark_hello_sent()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(b.mark_hello_sent()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.receive(hello_b)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(b.receive(hello_a)));
    EXPECT_EQ(std::get<HandshakeError>(a.receive(hello_b)), HandshakeError::InvalidState);
    const auto confirm_a = std::get<Bytes>(a.confirmation());
    const auto confirm_b = std::get<Bytes>(b.confirmation());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.mark_confirmation_sent()));
    EXPECT_FALSE(a.ready());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(b.receive(confirm_a)));
    EXPECT_FALSE(b.ready());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(b.mark_confirmation_sent()));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.receive(confirm_b)));
    EXPECT_TRUE(a.ready());
    EXPECT_TRUE(b.ready());
    EXPECT_EQ(std::get<HandshakeError>(a.receive(confirm_b)), HandshakeError::InvalidState);
    EXPECT_EQ(a.message_limit(), 80U);
    EXPECT_EQ(b.message_limit(), 80U);
}

TEST(Handshake, RejectsWrongChallengeAndPrematureTransitions) {
    Fixture f;
    HandshakeExchange a(f.offerer, f.context(), HandshakeRole::Offerer, {std::byte{7}}, 100);
    EXPECT_EQ(std::get<HandshakeError>(a.confirmation()), HandshakeError::InvalidState);
    EXPECT_EQ(std::get<HandshakeError>(a.mark_confirmation_sent()), HandshakeError::InvalidState);
    EXPECT_FALSE(a.message_limit());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.mark_hello_sent()));
    const auto hello = std::get<Bytes>(
        encode_handshake(f.answerer, f.context(),
                         {HandshakeKind::Hello, HandshakeRole::Answerer, 80, {std::byte{8}}, {}}));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.receive(hello)));
    const auto wrong = std::get<Bytes>(encode_handshake(
        f.answerer, f.context(),
        {HandshakeKind::Confirm, HandshakeRole::Answerer, 80, {std::byte{8}}, {std::byte{9}}}));
    EXPECT_EQ(std::get<HandshakeError>(a.receive(wrong)), HandshakeError::InvalidState);
    EXPECT_FALSE(a.ready());
    const auto old_confirmation = std::get<Bytes>(encode_handshake(
        f.answerer, f.context(),
        {HandshakeKind::Confirm, HandshakeRole::Answerer, 80, {std::byte{8}}, {std::byte{7}}}));
    HandshakeExchange fresh(f.offerer, f.context(), HandshakeRole::Offerer, {std::byte{10}}, 100);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(fresh.mark_hello_sent()));
    EXPECT_EQ(std::get<HandshakeError>(fresh.receive(old_confirmation)),
              HandshakeError::InvalidState);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(fresh.receive(hello)));
    EXPECT_EQ(std::get<HandshakeError>(fresh.receive(old_confirmation)),
              HandshakeError::InvalidState);
    EXPECT_THROW((HandshakeExchange{f.answerer, f.context(), HandshakeRole::Offerer,
                                    HandshakeNonce{std::byte{7}}, 1}),
                 std::invalid_argument);
}
