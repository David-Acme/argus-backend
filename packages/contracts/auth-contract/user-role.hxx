#pragma once

#include <cstdint>
#include <string>

enum class UserRole : uint8_t
{
  Owner = 0,
  Resident,
  Guard,
  Guest
};

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
  }
  return "guest";
}

inline UserRole userRoleFromString(const std::string& s)
{
  if (s == "owner")
    return UserRole::Owner;
  if (s == "resident")
    return UserRole::Resident;
  if (s == "guard")
    return UserRole::Guard;
  return UserRole::Guest;
}
