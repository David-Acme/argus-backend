#pragma once

#include <string>
#include <string_view>

namespace tapo_day_night
{
inline std::string_view toDevice(std::string_view mode)
{
  if (mode == "night")
    return "on";
  if (mode == "day")
    return "off";
  return "auto";
}

inline std::string fromDevice(std::string_view value)
{
  if (value == "on")
    return "night";
  if (value == "off")
    return "day";
  return std::string(value);
}
}
