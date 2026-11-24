#pragma once

#include "backend_loop.hpp"
#include "record_client.hpp"

#include <memory>

namespace p2pdc::detail {

// A record store over one shared network client, with its own limits. Calls may come from any
// thread; the work runs on the client's loop, and so do the callbacks. Fails with NotRunning
// once the loop has stopped.
[[nodiscard]] RecordStore make_client_record_store(std::shared_ptr<BackendLoop> loop,
                                                   std::shared_ptr<RecordClient> client,
                                                   StoreLimits limits);

} // namespace p2pdc::detail
