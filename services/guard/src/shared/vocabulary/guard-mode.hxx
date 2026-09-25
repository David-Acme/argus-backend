#pragma once

#include <cstdint>
#include <string>

enum class GuardMode : uint8_t
{
  Home = 0,
  Away,
  Night,
  Armed
};

inline std::string guardModeToString(GuardMode mode)
{
  switch (mode) {
    case GuardMode::Home:
      return "home";
    case GuardMode::Away:
      return "away";
    case GuardMode::Night:
      return "night";
    case GuardMode::Armed:
      return "armed";
  }
  return "home";
}

inline GuardMode guardModeFromString(const std::string& value)
{
  if (value == "away")
    return GuardMode::Away;
  if (value == "night")
    return GuardMode::Night;
  if (value == "armed")
    return GuardMode::Armed;
  return GuardMode::Home;
}
