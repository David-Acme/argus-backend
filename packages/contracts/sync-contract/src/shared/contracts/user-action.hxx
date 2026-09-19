#pragma once

#include <cstdint>
#include <string>

enum class UserAction : uint8_t
{
  Create = 0,
  Read,
  Update,
  Delete
};

inline std::string userActionToString(UserAction a)
{
  switch (a) {
    case UserAction::Read:
      return "read";
    case UserAction::Update:
      return "update";
    case UserAction::Delete:
      return "delete";
    default:
      return "create";
  }
}

inline UserAction userActionFromString(const std::string& s)
{
  if (s == "read")
    return UserAction::Read;
  if (s == "update")
    return UserAction::Update;
  if (s == "delete")
    return UserAction::Delete;
  return UserAction::Create;
}
