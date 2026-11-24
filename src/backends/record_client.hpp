#pragma once

#include "backend_records.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <variant>

namespace p2pdc::detail {

// One server as a record backend, the native form of the browser's record backend contract
// (docs/DISCOVERY_BACKENDS.md). A client is shared by every store of a node that uses the
// server, each passing its own limits. Every method and callback runs on the node's backend
// loop. Whatever a server returns is untrusted: records beyond the limits are dropped.
class RecordClient {
  public:
    using Done = std::function<void(std::optional<BackendFailure>)>;
    using Found = std::function<void(std::variant<StoreBatch, BackendFailure>)>;
    struct Listener {
        std::function<void(BackendRecord)> record;
        // Once: true when the server confirmed the watch, false when it ended first.
        std::function<void(bool)> ready;
        // Once, unless unlistened: nullopt when closed, else why the watch failed.
        std::function<void(std::optional<BackendFailure>)> ended;
    };
    using WatchId = std::uint64_t;

    virtual ~RecordClient() = default;
    virtual void put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) = 0;
    virtual void get(const RecordKey &key, StoreLimits limits, bool by_content, Found found) = 0;
    virtual WatchId listen(const RecordKey &key, StoreLimits limits, Listener listener) = 0;
    // Ends a watch without calling its listener again.
    virtual void unlisten(WatchId watch) = 0;
    // Closes the connection; every operation in flight fails.
    virtual void close() = 0;
};

} // namespace p2pdc::detail
