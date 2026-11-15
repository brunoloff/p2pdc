#include "incoming_offer_resolver.hpp"

#include "p2pdc/attempt_guard.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
bool nonzero(ByteView value) {
    return std::any_of(value.begin(), value.end(), [](std::byte b) { return b != std::byte{}; });
}

bool valid_peer(const Advertisement &peer, const IncomingOfferLimits &limits, std::uint64_t now) {
    const auto &f = peer.fields;
    return nonzero(peer.signing_public_key) && nonzero(f.instance_id) &&
           nonzero(f.recipient_public_key) && nonzero(f.signaling_inbox) &&
           f.expires_at > f.issued_at &&
           f.expires_at - f.issued_at <= limits.max_advertisement_lifetime &&
           (f.issued_at <= now || f.issued_at - now <= limits.clock_skew) &&
           (now < f.expires_at || now - f.expires_at < limits.clock_skew);
}

std::uint64_t add_saturating(std::uint64_t a, std::uint64_t b) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return b > maximum - a ? maximum : a + b;
}

std::variant<Advertisement, IncomingOfferError> find_peer(const SignalingEnvelope &offer,
                                                          std::span<const Advertisement> peers,
                                                          const IncomingOfferLimits &limits,
                                                          std::uint64_t now) {
    std::optional<Advertisement> match;
    for (const auto &peer : peers) {
        if (peer.signing_public_key != offer.sender_signing_key ||
            peer.fields.instance_id != offer.fields.sender_instance)
            continue;
        if (!valid_peer(peer, limits, now))
            return IncomingOfferError::InvalidAdvertisement;
        if (match && *match != peer)
            return IncomingOfferError::AmbiguousPeer;
        match = peer;
    }
    return match ? std::variant<Advertisement, IncomingOfferError>(*match)
                 : std::variant<Advertisement, IncomingOfferError>(IncomingOfferError::UnknownPeer);
}
} // namespace

IncomingOfferResolver::IncomingOfferResolver(IncomingOfferLimits limits,
                                             std::shared_ptr<IncomingOfferReplay> replay)
    : limits_(limits),
      replay_(replay ? std::move(replay) : std::make_shared<IncomingOfferReplay>()) {
    if (!limits.max_attempts || !limits.max_offer_lifetime || !limits.max_advertisement_lifetime ||
        !limits.max_payload_size || !limits.max_forward_jump || limits.max_forward_jump > 64 ||
        limits.max_payload_size > std::numeric_limits<std::uint32_t>::max() ||
        limits.max_payload_size >
            std::numeric_limits<std::size_t>::max() - signaling_wire_overhead) {
        throw std::invalid_argument("invalid incoming offer limits");
    }
}

std::optional<IncomingOfferError> IncomingOfferResolver::prune(std::uint64_t now) {
    if (replay_->last_now_ && now < *replay_->last_now_)
        return IncomingOfferError::ClockRegressed;
    replay_->last_now_ = now;
    std::erase_if(replay_->attempts_, [now](const auto &entry) { return now > entry.second; });
    return std::nullopt;
}

IncomingOfferResult
IncomingOfferResolver::resolve(ByteView wire, const RecipientKeyPair &recipient_key,
                               const SignalingRecipient &recipient, ByteView capability,
                               std::span<const Advertisement> verified_peers, std::uint64_t now) {
    auto group = derive_discovery_key(capability);
    if (auto *error = std::get_if<CryptoError>(&group))
        return *error;
    if (group_ && (*group_ != std::get<DiscoveryKey>(group) || *recipient_ != recipient ||
                   *recipient_box_ != recipient_key.public_key()))
        return IncomingOfferError::ContextMismatch;
    if (auto error = prune(now))
        return *error;
    auto decoded = decode_signaling(
        wire, recipient_key, recipient, capability,
        {now, limits_.max_offer_lifetime, limits_.clock_skew, limits_.max_payload_size});
    if (auto *error = std::get_if<SignalingError>(&decoded))
        return *error;
    if (auto *error = std::get_if<CryptoError>(&decoded))
        return *error;
    auto envelope = std::get<SignalingEnvelope>(std::move(decoded));
    if (envelope.fields.kind != SignalingKind::Offer)
        return IncomingOfferError::NotOffer;
    if (!nonzero(envelope.fields.attempt_id))
        return IncomingOfferError::InvalidAttempt;
    auto peer = find_peer(envelope, verified_peers, limits_, now);
    if (auto *error = std::get_if<IncomingOfferError>(&peer))
        return *error;
    auto result = admit(std::move(envelope), std::get<Advertisement>(std::move(peer)), wire, now);
    if (std::holds_alternative<IncomingOffer>(result)) {
        group_ = std::get<DiscoveryKey>(group);
        recipient_ = recipient;
        recipient_box_ = recipient_key.public_key();
    }
    return result;
}

IncomingOfferResult IncomingOfferResolver::admit(SignalingEnvelope envelope, Advertisement peer,
                                                 ByteView wire, std::uint64_t now) {
    AttemptGuard guard(envelope.sender_signing_key, envelope.fields.sender_instance,
                       envelope.fields.attempt_id, limits_.max_forward_jump);
    if (guard.accept(envelope) != AttemptDecision::Accepted)
        return IncomingOfferError::InvalidSequence;
    IncomingOfferReplay::Key key{envelope.sender_signing_key, envelope.fields.sender_instance,
                                 envelope.fields.attempt_id};
    if (replay_->attempts_.contains(key))
        return IncomingOfferError::Duplicate;
    if (replay_->attempts_.size() >= limits_.max_attempts)
        return IncomingOfferError::Full;
    IncomingOffer result{std::move(peer), std::move(envelope), Bytes(wire.begin(), wire.end())};
    const auto retain = add_saturating(
        add_saturating(add_saturating(now, limits_.max_offer_lifetime), limits_.clock_skew),
        limits_.clock_skew);
    replay_->attempts_.emplace(std::move(key), retain);
    return result;
}

std::size_t IncomingOfferResolver::size() const { return replay_->attempts_.size(); }
} // namespace p2pdc::detail
