// Keeps libdatachannel's verbose log for the test that is running and prints it only when that
// test fails, so a connection that does not come up on some runner shows why. With
// P2PDC_TEST_STACKS_ON_FAILURE set, the first failure in a test also has gdb print every
// thread's stack while whatever stalled is still stalled.
#include <gtest/gtest.h>
#include <rtc/global.hpp>

#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr std::size_t max_lines = 30000;

// Never freed: libdatachannel's threads may still log while the process exits.
struct Lines {
    std::mutex mutex;
    std::deque<std::string> lines;
};
Lines &kept = *new Lines;

void print_stacks() {
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    const auto command =
        "gdb --batch -p " + std::to_string(pid) + " -ex \"thread apply all bt\" 1>&2";
    std::cerr << "--- stacks of every thread (" << command << ") ---" << std::endl;
    if (const int status = std::system(command.c_str()); status != 0)
        std::cerr << "--- gdb ended with status " << status << " ---" << std::endl;
}

class RtcLogOnFailure final : public ::testing::EmptyTestEventListener {
  public:
    void OnTestStart(const ::testing::TestInfo &) override {
        std::lock_guard lock(kept.mutex);
        kept.lines.clear();
        stacks_printed_ = false;
    }
    void OnTestPartResult(const ::testing::TestPartResult &result) override {
        if (!result.failed() || stacks_printed_ || !std::getenv("P2PDC_TEST_STACKS_ON_FAILURE"))
            return;
        stacks_printed_ = true;
        print_stacks();
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

  private:
    bool stacks_printed_ = false;
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
