#pragma once

#include "config.hpp"
#include <rtc/candidate.hpp>

namespace native_smoke {
enum class Route { Unknown, Direct, TurnRelayed };
Route classify_pair(bool selected, rtc::Candidate::Type local, rtc::Candidate::Type remote);
bool run_smoke(const Config &config);
} // namespace native_smoke
