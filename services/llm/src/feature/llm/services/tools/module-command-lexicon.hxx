#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace module_command
{

enum class Group : std::uint8_t
{
  CreateVerb,
  StrongCreate,
  LeadAgenda,
  EventNoun,
  CalendarNoun,
  TimeMark,
  CancelVerb,
  QueryMarker,
  FreeMarker,
  HaveScheduled,
  TaskNoun,
  NewMark,
  CompleteVerb,
  CompleteStrong,
  MarkVerb,
  DoneMark,
  ProjectNoun,
  ProjectVerb,
  ScreenWord,
  ModuleNoun,
  ModulePlural,
  ProjectVeto,
  BookingObject,
  EnableVerb,
  DisableVerb,
  PurgeVerb,
  DataNoun,
  RequestVerb,
  OwnerNoun,
  ExplainMarker,
  ExplainGeneric,
  ReminderNoun,
  Decline,
  Filler,
  Count
};

inline constexpr std::size_t kGroupCount = static_cast<std::size_t>(Group::Count);

struct Table
{
  std::string_view language;
  std::array<std::span<const std::string_view>, kGroupCount> groups;
};

[[nodiscard]] std::span<const Table> tables();

}
