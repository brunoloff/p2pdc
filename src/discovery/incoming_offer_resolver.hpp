#pragma once

#include "p2pdc/advertisement.h"
#include "p2pdc/discovery_key.h"
#include "p2pdc/signaling.h"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>

namespace p2pdc::detail {

enum class IncomingOfferError {
    NotOffer,
    UnknownPeer,
    AmbiguousPeer,
    InvalidAdvertisement,
    InvalidAttempt,
    InvalidSequence,
    Duplicate,
    Full,
    ClockRegressed,
    ContextMismatch
};

struct IncomingOffer {
    Advertisement peer;
    SignalingEnvelope envelope;
    Bytes wire;
};

using IncomingOfferResult =
    std::variant<IncomingOffer, IncomingOfferError, SignalingError, CryptoError>;

// Shared by successive listeners for one lease. Only the owning Node's
// serialized executor may access it; entries remain bounded and expire.
class IncomingOfferReplay {
    friend class IncomingOfferResolver;
    using Key = std::tuple<SigningPublicKey, SignalingInstanceId, AttemptId>;
    std::optional<std::uint64_t> last_now_;
    std::map<Key, std::uint64_t> attempts_;
};

struct IncomingOfferLimits {
    std::size_t max_attempts;
    std::uint64_t max_offer_lifetime;
    std::uint64_t max_advertisement_lifetime;
    std::uint64_t clock_skew;
    std::size_t max_payload_size;
    std::uint8_t max_forward_jump;
};

// Single serialized caller. Advertisements must already be authenticated under
// this same capability and be current entries from the discovery tracker; the
// caller must also bound the size of the supplied advertisement list.
// Resolving an offer establishes cryptographic identity, not application consent.
// Unknown peers may be retried after discovery catches up; only accepted offers
// consume bounded replay entries. These entries are in-memory and expire; this
// does not provide durable replay protection or prevent malicious ID reuse.
// The first accepted offer binds the resolver to its capability and local keys;
// later context changes are rejected even after replay entries expire. Container
// allocation failures propagate and must be handled by the owner.
class IncomingOfferResolver {
  public:
    explicit IncomingOfferResolver(IncomingOfferLimits limits,
                                   std::shared_ptr<IncomingOfferReplay> replay = {});
    [[nodiscard]] IncomingOfferResult resolve(ByteView wire, const RecipientKeyPair &recipient_key,
                                              const SignalingRecipient &recipient,
                                              ByteView capability,
                                              std::span<const Advertisement> verified_peers,
                                              std::uint64_t now);
    [[nodiscard]] std::optional<IncomingOfferError> prune(std::uint64_t now);
    [[nodiscard]] std::size_t size() const;

  private:
    [[nodiscard]] IncomingOfferResult admit(SignalingEnvelope envelope, Advertisement peer,
                                            ByteView wire, std::uint64_t now);
    IncomingOfferLimits limits_;
    std::shared_ptr<IncomingOfferReplay> replay_;
    std::optional<DiscoveryKey> group_;
    std::optional<SignalingRecipient> recipient_;
    std::optional<RecipientBoxPublicKey> recipient_box_;
};
} // namespace p2pdc::detail
