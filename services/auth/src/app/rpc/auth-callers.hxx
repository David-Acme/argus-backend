#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace auth_callers
{

inline constexpr std::array<std::string_view, 7> kExpected{
    "camera",       "guard",    "identity", "notification",
    "productivity", "settings", "sync"};

inline std::vector<std::string> expected()
{
  return {kExpected.begin(), kExpected.end()};
}

}
