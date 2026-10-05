#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace identity_callers
{

inline constexpr std::array<std::string_view, 8> kExpected{
    "auth",         "camera",       "guard", "llm",
    "notification", "productivity", "sync",  "voice"};

inline constexpr std::array<std::string_view, 1> kRegisterUser{"auth"};
inline constexpr std::array<std::string_view, 2> kUpdateUser{"auth", "voice"};
inline constexpr std::array<std::string_view, 2> kIdentifyPerson{"auth",
                                                                 "camera"};
inline constexpr std::array<std::string_view, 1> kCameraSighting{"camera"};
inline constexpr std::array<std::string_view, 1> kGuardCuration{"guard"};
inline constexpr std::array<std::string_view, 1> kPullTable{"sync"};
inline constexpr std::array<std::string_view, 1> kVoiceprint{"voice"};

inline std::vector<std::string> expected()
{
  return {kExpected.begin(), kExpected.end()};
}

}
