#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class SafetyAlertKind : uint8_t
{
  Panic = 0,
  Duress
};

inline std::string safetyAlertKindToString(SafetyAlertKind kind)
{
  switch (kind) {
    case SafetyAlertKind::Panic:
      return "panic";
    case SafetyAlertKind::Duress:
      return "duress";
  }
  return "panic";
}

inline std::optional<SafetyAlertKind> safetyAlertKindFromString(std::string_view value)
{
  if (value == "panic")
    return SafetyAlertKind::Panic;
  if (value == "duress")
    return SafetyAlertKind::Duress;
  return std::nullopt;
}
