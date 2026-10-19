#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ViewCheckState : uint8_t
{
  Pending = 0,
  Accepted,
  Refused
};

inline std::string viewCheckStateToString(ViewCheckState state)
{
  switch (state) {
    case ViewCheckState::Pending:
      return "pending";
    case ViewCheckState::Accepted:
      return "accepted";
    case ViewCheckState::Refused:
      return "refused";
  }
  return "pending";
}

inline std::optional<ViewCheckState> viewCheckStateFromString(std::string_view value)
{
  if (value == "pending")
    return ViewCheckState::Pending;
  if (value == "accepted")
    return ViewCheckState::Accepted;
  if (value == "refused")
    return ViewCheckState::Refused;
  return std::nullopt;
}
