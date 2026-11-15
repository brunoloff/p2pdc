#include "../src/browser/browser_public_config.hpp"
#include "../src/browser/browser_record_store.hpp"

#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Fixture {
    std::shared_ptr<Identity> local_signing = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    std::shared_ptr<Identity> remote_signing = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2}))));
    std::shared_ptr<RecipientKeyPair> local_box = std::make_shared<RecipientKeyPair>(
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3}))));
    std::shared_ptr<RecipientKeyPair> remote_box = std::make_shared<RecipientKeyPair>(
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{4}))));
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{5});
    std::shared_ptr<EndpointIdentity> identity = std::make_shared<EndpointIdentity>();
    ConnectionAdmission admission;
    Fixture();
    [[nodiscard]] IncomingOffer offer() const;
};

Fixture::Fixture()
    : admission{make_browser_record_store("http://127.0.0.1:49412", {32768, 128, 1048576}),
                {},
                {},
                capability,
                60,
                15} {
    identity->signing = local_signing;
    identity->recipient = local_box;
    admission.local.identity = identity;
    admission.local.fields.recipient_public_key = local_box->public_key();
    admission.local.fields.instance_id[0] = std::byte{1};
    admission.local.fields.signaling_inbox[0] = std::byte{3};
    admission.local.fields.issued_at = 90;
    admission.local.fields.expires_at = 150;
    admission.remote.signing_public_key = remote_signing->public_key();
    admission.remote.fields.recipient_public_key = remote_box->public_key();
    admission.remote.fields.instance_id[0] = std::byte{2};
    admission.remote.fields.signaling_inbox[0] = std::byte{4};
    admission.remote.fields.issued_at = 90;
    admission.remote.fields.expires_at = 150;
}
IncomingOffer Fixture::offer() const {
    SignalingFields fields{SignalingKind::Offer,
                           admission.remote.fields.instance_id,
                           {local_signing->public_key(), admission.local.fields.instance_id},
                           {std::byte{6}},
                           0,
                           110,
                           140,
                           {std::byte{7}}};
    auto encoded = encode_signaling(*remote_signing, fields, local_box->public_key(), *capability,
                                    {100, 60, 15, 32768});
    return {admission.remote,
            {remote_signing->public_key(), fields},
            std::get<Bytes>(std::move(encoded))};
}
bool rejects(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}
} // namespace

bool browser_public_config_smoke() {
    Fixture fixture;
    auto make = [&](const IncomingOffer *offer, std::uint64_t now) {
        return make_browser_public_config(fixture.admission, offer, {}, MemoryBudget(4194304),
                                          65536, [now] { return now; });
    };
    const auto first = make(nullptr, 100);
    const auto second = make(nullptr, 100);
    if (first.secure.binding.role != HandshakeRole::Offerer ||
        first.secure.binding.attempt == second.secure.binding.attempt ||
        first.secure.clock_skew != 15 || first.secure.max_message_bytes != 65536)
        return false;
    auto offer = fixture.offer();
    const auto accepted = make(&offer, 100);
    if (accepted.secure.binding.role != HandshakeRole::Answerer ||
        accepted.secure.binding.attempt != offer.envelope.fields.attempt_id ||
        !accepted.accepted_offer || *accepted.accepted_offer != offer.wire)
        return false;
    offer.wire.back() ^= std::byte{1};
    return rejects([&] { (void)make(&offer, 100); }) && rejects([&] { (void)make(nullptr, 150); });
}
