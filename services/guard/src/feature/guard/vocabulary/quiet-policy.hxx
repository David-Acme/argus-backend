#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class QuietPolicy : uint8_t
{
  Inherit = 0,
  Custom,
  Off
};

inline std::string quietPolicyToString(QuietPolicy policy)
{
  switch (policy) {
    case QuietPolicy::Inherit:
      return "inherit";
    case QuietPolicy::Custom:
      return "custom";
    case QuietPolicy::Off:
      return "off";
  }
  return "inherit";
}

inline std::optional<QuietPolicy> quietPolicyFromString(std::string_view value)
{
  if (value == "inherit")
    return QuietPolicy::Inherit;
  if (value == "custom")
    return QuietPolicy::Custom;
  if (value == "off")
    return QuietPolicy::Off;
  return std::nullopt;
}
