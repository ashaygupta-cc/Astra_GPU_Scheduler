#pragma once

#include <string_view>

namespace astra {

inline constexpr std::string_view ASTRA_VERSION = "0.1.0-alpha";
inline constexpr unsigned int ASTRA_VERSION_MAJOR = 0;
inline constexpr unsigned int ASTRA_VERSION_MINOR = 1;
inline constexpr unsigned int ASTRA_VERSION_PATCH = 0;

inline constexpr std::string_view ASTRA_BANNER = R"(
   ___    ____  ______  ____    ___ 
  / _ |  / __/_/_  __/ / _  |  / _ |
 / __ | _\ \   / /   / , _/  / __ |
/_/ |_|/___/  /_/   /_/|_|  /_/ |_|
 Autonomous CPU/GPU Job Scheduler
)";

} // namespace astra
