#pragma once

#include <cstdint>

// Why a share could not be granted; the controller maps each case to its own
// status.
enum class MembershipError : uint8_t
{
  None = 0,
  ParentNotFound,
  UserNotFound,
  UserNotAllowed,
  SelfShare
};
