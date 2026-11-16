#pragma once

#include <exception>
#include <iostream>

namespace p2pdc::detail {

// Application callbacks must not throw. When one does, its owner records a
// Callback error for the application and then calls this from the catch block
// instead of rethrowing: executors such as the bundled Runtime have no handler,
// so a rethrow would terminate the process.
inline void log_callback_exception(const char *callback) noexcept {
    try {
        throw;
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: " << callback << " callback threw: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "p2pdc: " << callback << " callback threw a non-standard exception\n";
    }
}

} // namespace p2pdc::detail
