#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sync_control_callers
{

inline constexpr std::array<std::string_view, 2> kExpected{"identity",
                                                           "notification"};

inline constexpr std::array<std::string_view, 1> kRoomControl{"identity"};
inline constexpr std::array<std::string_view, 1> kCloseCallRoom{"notification"};
inline constexpr std::array<std::string_view, 2> kEmitToUser{"identity",
                                                             "notification"};
inline constexpr std::array<std::string_view, 1> kWatchEmit{"voice"};

inline std::vector<std::string> expected()
{
  return {kExpected.begin(), kExpected.end()};
}

}
