#include "collision_registry.hpp"
#include "connection_admission.hpp"

#include <sodium.h>

#include <algorithm>
#include <tuple>

namespace p2pdc::detail {
namespace {
// Capabilities are secrets; compare them in constant time.
bool same_secret(const Bytes &first, const Bytes &second) {
    return first.size() == second.size() &&
           sodium_memcmp(first.data(), second.data(), first.size()) == 0;
}
} // namespace

std::vector<std::shared_ptr<ConnectionBackend>>
CollisionRegistry::opposing(const ConnectionAdmission &admission, ConnectionDirection direction) {
    std::erase_if(entries_, [](const auto &entry) {
        auto connection = entry.connection.lock();
        return !connection || connection->state() == ConnectionState::Closed ||
               connection->state() == ConnectionState::Failed || entry.capability.expired();
    });
    std::vector<std::shared_ptr<ConnectionBackend>> result;
    for (const auto &entry : entries_) {
        auto capability = entry.capability.lock();
        if (entry.direction != direction && capability &&
            same_secret(*capability, *admission.capability) &&
            entry.local_key == admission.local.identity->signing->public_key() &&
            entry.local_instance == admission.local.fields.instance_id &&
            entry.remote_key == admission.remote.signing_public_key &&
            entry.remote_instance == admission.remote.fields.instance_id)
            if (auto connection = entry.connection.lock())
                result.push_back(std::move(connection));
    }
    return result;
}
void CollisionRegistry::remember(const ConnectionAdmission &admission,
                                 ConnectionDirection direction,
                                 const std::shared_ptr<ConnectionBackend> &connection) {
    entries_.push_back({connection, admission.capability,
                        admission.local.identity->signing->public_key(),
                        admission.remote.signing_public_key, admission.local.fields.instance_id,
                        admission.remote.fields.instance_id, direction});
}
bool CollisionRegistry::preferred(const ConnectionAdmission &admission,
                                  ConnectionDirection direction) {
    const auto local = std::make_tuple(admission.local.identity->signing->public_key(),
                                       admission.local.fields.instance_id);
    const auto remote =
        std::make_tuple(admission.remote.signing_public_key, admission.remote.fields.instance_id);
    return direction == ConnectionDirection::Outgoing ? local < remote : remote < local;
}
} // namespace p2pdc::detail
