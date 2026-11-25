#include "../src/discovery/peer_admission.hpp"
#ifdef __EMSCRIPTEN__
#include "../src/browser/browser_record_store.hpp"
#include "../src/discovery/connection_admission.hpp"
#endif

#include <memory>
#include <optional>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct FakeDiscovery final : detail::DiscoveryBackend {
    bool active = true;
    std::optional<LeaseSnapshot> local;
    std::optional<PeerSnapshot> remote;
    void close() override { active = false; }
    bool healthy() const override { return active; }
    std::optional<LeaseSnapshot> lease_snapshot() override { return local; }
    std::optional<PeerSnapshot> peer_snapshot(const Advertisement &value) override {
        return remote && remote->peer == value ? remote : std::nullopt;
    }
};

bool error_is(const PeerAdmissionResult &result, AdmissionError expected) {
    const auto *error = std::get_if<AdmissionError>(&result);
    return error && *error == expected;
}

struct AdmissionFixture {
    FakeDiscovery lease, watch;
    std::shared_ptr<EndpointIdentity> identity = std::make_shared<EndpointIdentity>();
    std::shared_ptr<Bytes> group = std::make_shared<Bytes>(32, std::byte{5});
    Advertisement peer{};

    AdmissionFixture();
    bool accepts();
    bool rejects_unavailable();
    bool rejects_mismatches();
};

AdmissionFixture::AdmissionFixture() {
    identity->signing = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    identity->recipient = std::make_shared<RecipientKeyPair>(
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3}))));
    const auto remote = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    AdvertisementFields fields{};
    fields.instance_id[0] = std::byte{1};
    fields.recipient_public_key = identity->recipient->public_key();
    peer.signing_public_key = remote.public_key();
    peer.fields.instance_id[0] = std::byte{2};
    lease.local = LeaseSnapshot{{identity, fields}, group, DiscoveryProtocol::RotatingKeysV2};
    watch.remote = PeerSnapshot{peer, group, 300, 30, DiscoveryProtocol::RotatingKeysV2};
}

bool AdmissionFixture::accepts() {
    auto result = resolve_peer_context(lease, watch, peer);
    const auto *value = std::get_if<PeerAdmission>(&result);
    return value && value->local.identity == identity && value->remote == peer &&
           value->capability == group && value->remote_max_lifetime == 300 &&
           value->remote_clock_skew == 30;
}

bool AdmissionFixture::rejects_unavailable() {
    const auto saved = lease.local;
    lease.local.reset();
    if (!error_is(resolve_peer_context(lease, watch, peer), AdmissionError::LocalNotPublished))
        return false;
    lease.local = saved;
    lease.local->endpoint.identity.reset();
    if (!error_is(resolve_peer_context(lease, watch, peer), AdmissionError::Closed))
        return false;
    lease.local = saved;
    auto stale = peer;
    stale.fields.instance_id[0] ^= std::byte{1};
    if (!error_is(resolve_peer_context(lease, watch, stale), AdmissionError::UnverifiedPeer))
        return false;
    watch.close();
    return error_is(resolve_peer_context(lease, watch, peer), AdmissionError::Closed);
}

bool AdmissionFixture::rejects_mismatches() {
    watch.remote->capability = std::make_shared<Bytes>(31, std::byte{5});
    if (!error_is(resolve_peer_context(lease, watch, peer), AdmissionError::DifferentGroup))
        return false;
    watch.remote->capability = std::make_shared<Bytes>(32, std::byte{6});
    if (!error_is(resolve_peer_context(lease, watch, peer), AdmissionError::DifferentGroup))
        return false;
    watch.remote->capability = group;
    watch.remote->protocol = DiscoveryProtocol::FixedKeyV1;
    if (!error_is(resolve_peer_context(lease, watch, peer), AdmissionError::DifferentGroup))
        return false;
    watch.remote->protocol = DiscoveryProtocol::RotatingKeysV2;
    peer.signing_public_key = identity->signing->public_key();
    peer.fields.instance_id = lease.local->endpoint.fields.instance_id;
    watch.remote->peer = peer;
    return error_is(resolve_peer_context(lease, watch, peer), AdmissionError::Self);
}
} // namespace

bool portable_admission_smoke() {
    AdmissionFixture valid, unavailable, mismatched;
    if (!valid.accepts() || !unavailable.rejects_unavailable() || !mismatched.rejects_mismatches())
        return false;
#ifdef __EMSCRIPTEN__
    auto store = make_browser_record_store("http://127.0.0.1:49412", {32768, 128, 1048576});
    auto accepted = resolve_owned_context(store, valid.lease, valid.watch, valid.peer);
    if (!std::holds_alternative<ConnectionAdmission>(accepted))
        return false;
    valid.watch.close();
    auto rejected = resolve_owned_context(store, valid.lease, valid.watch, valid.peer);
    const auto *error = std::get_if<AdmissionError>(&rejected);
    return error && *error == AdmissionError::Closed;
#else
    return true;
#endif
}
