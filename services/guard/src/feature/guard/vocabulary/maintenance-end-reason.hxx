#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class MaintenanceEndReason : uint8_t
{
  None = 0,
  Done,
  Expired,
  UnknownPerson,
  Replaced
};

inline std::string maintenanceEndReasonToString(MaintenanceEndReason reason)
{
  switch (reason) {
    case MaintenanceEndReason::None:
      return "";
    case MaintenanceEndReason::Done:
      return "done";
    case MaintenanceEndReason::Expired:
      return "expired";
    case MaintenanceEndReason::UnknownPerson:
      return "unknown_person";
    case MaintenanceEndReason::Replaced:
      return "replaced";
  }
  return "";
}

inline std::optional<MaintenanceEndReason> maintenanceEndReasonFromString(std::string_view value)
{
  if (value.empty())
    return MaintenanceEndReason::None;
  if (value == "done")
    return MaintenanceEndReason::Done;
  if (value == "expired")
    return MaintenanceEndReason::Expired;
  if (value == "unknown_person")
    return MaintenanceEndReason::UnknownPerson;
  if (value == "replaced")
    return MaintenanceEndReason::Replaced;
  return std::nullopt;
}
