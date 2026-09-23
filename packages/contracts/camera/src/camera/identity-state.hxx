#pragma once

#include <cstdint>
#include <string>

enum class IdentityState : uint8_t
{
  Known = 0,
  Unrecognized,
  Unobservable
};

inline std::string identityStateToString(IdentityState state)
{
  switch (state) {
    case IdentityState::Known:
      return "known";
    case IdentityState::Unrecognized:
      return "unrecognized";
    case IdentityState::Unobservable:
      return "unobservable";
  }
  return "unrecognized";
}

inline IdentityState identityStateFromString(const std::string& s)
{
  if (s == "known")
    return IdentityState::Known;
  if (s == "unobservable")
    return IdentityState::Unobservable;
  return IdentityState::Unrecognized;
}
