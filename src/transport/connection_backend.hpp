#pragma once

#include "p2pdc/connection.h"

#include <optional>

namespace p2pdc::detail {
struct ConnectionAdmission;
struct IncomingOffer;

class ConnectionBackend {
  public:
    virtual ~ConnectionBackend() = default;
    virtual void close() = 0;
    virtual void supersede() = 0;
    [[nodiscard]] virtual ConnectionState state() const = 0;
    [[nodiscard]] virtual RouteInfo route() const = 0;
    [[nodiscard]] virtual std::optional<std::uint32_t> max_message_size() const = 0;
    [[nodiscard]] virtual SendStatus send(ByteView message) = 0;
    [[nodiscard]] virtual std::optional<ConnectError>
    start(const ConnectionAdmission &admission, IceConfig ice,
          const IncomingOffer *offer = nullptr) = 0;
};
} // namespace p2pdc::detail
