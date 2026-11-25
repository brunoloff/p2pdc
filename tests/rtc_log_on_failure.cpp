// Keeps libdatachannel's verbose log for the test that is running and prints it only when that
// test fails, so a connection that does not come up on some runner shows why.
#include <gtest/gtest.h>
#include <rtc/global.hpp>

#include <cstddef>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>

namespace {
constexpr std::size_t max_lines = 6000;

// Never freed: libdatachannel's threads may still log while the process exits.
struct Lines {
    std::mutex mutex;
    std::deque<std::string> lines;
};
Lines &kept = *new Lines;

class RtcLogOnFailure final : public ::testing::EmptyTestEventListener {
  public:
    void OnTestStart(const ::testing::TestInfo &) override {
        std::lock_guard lock(kept.mutex);
        kept.lines.clear();
    }
    void OnTestEnd(const ::testing::TestInfo &info) override {
        if (!info.result()->Failed())
            return;
        std::lock_guard lock(kept.mutex);
        std::cerr << "--- libdatachannel log of " << info.test_suite_name() << '.' << info.name()
                  << " (last " << kept.lines.size() << " lines) ---\n";
        for (const auto &line : kept.lines)
            std::cerr << line << '\n';
        std::cerr << "--- end of libdatachannel log ---\n";
    }
};

void keep(rtc::LogLevel level, std::string message) {
    std::lock_guard lock(kept.mutex);
    if (kept.lines.size() == max_lines)
        kept.lines.pop_front();
    kept.lines.push_back(std::to_string(static_cast<int>(level)) + ' ' + std::move(message));
}

// Registered before main() runs; gtest owns the listener from then on.
[[maybe_unused]] const bool registered = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(new RtcLogOnFailure);
    rtc::InitLogger(rtc::LogLevel::Verbose, keep);
    return true;
}();
} // namespace
