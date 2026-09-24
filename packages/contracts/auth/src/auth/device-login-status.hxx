#pragma once

#include <cstdint>
#include <string>

enum class DeviceLoginStatus : std::uint8_t
{
  Pending = 0,
  Approved,
  Expired
};

inline std::string deviceLoginStatusToString(DeviceLoginStatus status)
{
  switch (status) {
    case DeviceLoginStatus::Approved:
      return "approved";
    case DeviceLoginStatus::Expired:
      return "expired";
    default:
      return "pending";
  }
}

inline DeviceLoginStatus deviceLoginStatusFromString(const std::string& value)
{
  if (value == "approved")
    return DeviceLoginStatus::Approved;
  if (value == "expired")
    return DeviceLoginStatus::Expired;
  return DeviceLoginStatus::Pending;
}
