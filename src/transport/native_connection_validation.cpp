#include "native_public_connection.hpp"
#ifdef __EMSCRIPTEN__
#include "../browser/browser_ice_config.hpp"
#else
#include "native_ice_config.hpp"
#endif

#include <stdexcept>

namespace p2pdc::detail {

void validate_public_ice_settings(const IceConfig &ice) {
#ifdef __EMSCRIPTEN__
    (void)browser_ice_servers(ice);
#else
    (void)make_native_ice_config(ice);
#endif
}

void validate_public_connection_settings(const Executor &executor, const Scheduler &scheduler,
                                         const ConnectOptions &options,
                                         const ConnectionHandlers &handlers) {
    if (!executor || !scheduler || !handlers.on_state || !handlers.on_route ||
        !handlers.on_message || !handlers.on_writable || !handlers.on_error ||
        options.timeout.count() <= 0 || options.timeout > std::chrono::minutes(10) ||
        options.poll_interval.count() < 10 || options.poll_interval > options.timeout ||
        !options.max_pending_events || options.max_pending_events > 1024 ||
        !options.max_message_bytes || options.max_message_bytes > 4 * 1024 * 1024 ||
        options.max_pending_bytes < options.max_message_bytes ||
        options.max_pending_bytes > 4 * 1024 * 1024) {
        throw std::invalid_argument("invalid connection settings");
    }
}

} // namespace p2pdc::detail
