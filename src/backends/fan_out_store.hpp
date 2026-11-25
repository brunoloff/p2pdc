#pragma once

#include "../discovery/record_store.hpp"
#include "backend_loop.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace p2pdc::detail {

struct FanOutTiming {
    // After the first backend answers a lookup, how long the others get before the merged
    // answer goes out without them: a blocked backend must not hold discovery up.
    std::chrono::milliseconds lookup_grace{1500};
    // A failed watch on one backend is started again after a backoff, doubling up to the cap.
    std::chrono::milliseconds rewatch{2000};
    std::chrono::milliseconds rewatch_cap{60000};
    // How often pending watch registrations are checked.
    std::chrono::milliseconds registration_poll{50};
};

// Several separate meeting places used at once, a port of fan_out_store.mjs
// (docs/DISCOVERY_BACKENDS.md): a put goes to every store and succeeds when one does; a lookup
// asks every store and merges the answers, waiting lookup_grace for the others after the first;
// a watch watches every store, and one that fails is watched again after a backoff while the
// others carry on. Results from the stores are gathered on the loop, and every callback runs
// there. Takes two or more stores.
[[nodiscard]] RecordStore make_fan_out_store(std::shared_ptr<BackendLoop> loop,
                                             std::vector<RecordStore> stores, StoreLimits limits,
                                             FanOutTiming timing = {});

} // namespace p2pdc::detail
