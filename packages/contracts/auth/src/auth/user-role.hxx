#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class UserRole : uint8_t
{
  Owner = 0,
  Resident,
  Guard,
  Guest,
  Unknown = 255
};

inline constexpr std::string_view kUnknownRoleName = "unknown";

constexpr bool userRoleKnown(UserRole role)
{
  return role != UserRole::Unknown;
}

inline std::string userRoleToString(UserRole r)
{
  switch (r) {
    case UserRole::Owner:
      return "owner";
    case UserRole::Resident:
      return "resident";
    case UserRole::Guard:
      return "guard";
    case UserRole::Guest:
      return "guest";
    case UserRole::Unknown:
      break;
  }
  return std::string(kUnknownRoleName);
}

inline std::optional<UserRole> parseUserRole(std::string_view s)
{
  if (s == "owner")
    return UserRole::Owner;
  if (s == "resident")
    return UserRole::Resident;
  if (s == "guard")
    return UserRole::Guard;
  if (s == "guest")
    return UserRole::Guest;
  return std::nullopt;
}

inline UserRole userRoleFromString(const std::string& s)
{
  return parseUserRole(s).value_or(UserRole::Unknown);
}
