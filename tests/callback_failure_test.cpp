#include "../src/core/callback_failure.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace p2pdc::detail;

namespace {
std::string logged(const std::function<void()> &thrower) {
    std::ostringstream captured;
    auto *previous = std::cerr.rdbuf(captured.rdbuf());
    try {
        thrower();
    } catch (...) {
        log_callback_exception("on_test");
    }
    std::cerr.rdbuf(previous);
    return captured.str();
}
} // namespace

TEST(CallbackFailure, LogsStandardAndUnknownExceptionsWithoutRethrowing) {
    EXPECT_EQ(logged([] { throw std::runtime_error("boom"); }),
              "p2pdc: on_test callback threw: boom\n");
    EXPECT_EQ(logged([] { throw 7; }),
              "p2pdc: on_test callback threw a non-standard exception\n");
}
