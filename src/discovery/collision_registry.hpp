#pragma once

#include "../transport/connection_backend.hpp"
#include "p2pdc/advertisement.h"

#include <memory>
#include <vector>

namespace p2pdc::detail {
struct ConnectionAdmission;
enum class ConnectionDirection { Outgoing, Incoming };

class CollisionRegistry {
  public:
    [[nodiscard]] std::vector<std::shared_ptr<ConnectionBackend>>
    opposing(const ConnectionAdmission &admission, ConnectionDirection direction);
    void remember(const ConnectionAdmission &admission, ConnectionDirection direction,
                  const std::shared_ptr<ConnectionBackend> &connection);
    [[nodiscard]] static bool preferred(const ConnectionAdmission &admission,
                                        ConnectionDirection direction);

  private:
    struct Entry {
        std::weak_ptr<ConnectionBackend> connection;
        std::weak_ptr<const Bytes> capability;
        SigningPublicKey local_key, remote_key;
        InstanceId local_instance, remote_instance;
        ConnectionDirection direction;
    };
    std::vector<Entry> entries_;
};
} // namespace p2pdc::detail
