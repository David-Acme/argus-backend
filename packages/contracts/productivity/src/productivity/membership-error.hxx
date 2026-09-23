#pragma once

#include <cstdint>

enum class MembershipError : uint8_t
{
  None = 0,
  ParentNotFound,
  UserNotFound,
  UserNotAllowed,
  SelfShare
};
