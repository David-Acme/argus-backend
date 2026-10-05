#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

enum class GuardReason : uint8_t
{
  Weapon = 0,
  AfterHours,
  NobodyHome,
  Armed,
  Night,
  AlertZone,
  SeveralStrangers,
  RepeatVisits,
  Escalating,
  Lingering,
  FaceHidden,
  ExpectedGuest,
  WithResident,
  WithGuest,
  PublicHours,
  StaffHours,
  AreaInUse,
  Passerby,
  Brief,
  Watchlist
};

inline constexpr std::array<std::pair<GuardReason, std::string_view>, 20>
    kGuardReasonNames{{{GuardReason::Weapon, "weapon"},
                       {GuardReason::AfterHours, "after_hours"},
                       {GuardReason::NobodyHome, "nobody_home"},
                       {GuardReason::Armed, "armed"},
                       {GuardReason::Night, "night"},
                       {GuardReason::AlertZone, "alert_zone"},
                       {GuardReason::SeveralStrangers, "several_strangers"},
                       {GuardReason::RepeatVisits, "repeat_visits"},
                       {GuardReason::Escalating, "escalating"},
                       {GuardReason::Lingering, "lingering"},
                       {GuardReason::FaceHidden, "face_hidden"},
                       {GuardReason::ExpectedGuest, "expected_guest"},
                       {GuardReason::WithResident, "with_resident"},
                       {GuardReason::WithGuest, "with_guest"},
                       {GuardReason::PublicHours, "public_hours"},
                       {GuardReason::StaffHours, "staff_hours"},
                       {GuardReason::AreaInUse, "area_in_use"},
                       {GuardReason::Passerby, "passerby"},
                       {GuardReason::Brief, "brief"},
                       {GuardReason::Watchlist, "watchlist"}}};

inline std::string guardReasonToString(GuardReason reason)
{
  for (const auto& [value, name] : kGuardReasonNames) {
    if (value == reason)
      return std::string(name);
  }
  return "brief";
}

inline std::optional<GuardReason> guardReasonFromString(std::string_view value)
{
  for (const auto& [reason, name] : kGuardReasonNames) {
    if (name == value)
      return reason;
  }
  return std::nullopt;
}

inline bool guardReasonRaises(GuardReason reason)
{
  return reason == GuardReason::Watchlist ||
         static_cast<uint8_t>(reason) <=
             static_cast<uint8_t>(GuardReason::FaceHidden);
}
