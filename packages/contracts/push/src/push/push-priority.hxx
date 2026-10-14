#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class PushPriority : uint8_t
{
  Normal = 0,
  High = 1
};

inline std::string_view pushPriorityToString(PushPriority priority) noexcept
{
  switch (priority) {
    case PushPriority::Normal:
      return "normal";
    case PushPriority::High:
      return "high";
  }
  return "";
}

inline std::optional<PushPriority>
pushPriorityFromString(std::string_view name) noexcept
{
  if (name == "normal")
    return PushPriority::Normal;
  if (name == "high")
    return PushPriority::High;
  return std::nullopt;
}
