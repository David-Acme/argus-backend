#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class PushKind : uint8_t
{
  Alert = 1,
  Call = 2
};

inline std::string_view pushKindToString(PushKind kind) noexcept
{
  switch (kind) {
    case PushKind::Alert:
      return "alert";
    case PushKind::Call:
      return "call";
  }
  return "";
}

inline std::optional<PushKind> pushKindFromString(std::string_view name) noexcept
{
  if (name == "alert")
    return PushKind::Alert;
  if (name == "call")
    return PushKind::Call;
  return std::nullopt;
}
