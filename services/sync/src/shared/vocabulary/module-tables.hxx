#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <sync/table-name.hxx>

namespace module_tables
{
inline constexpr std::string_view kCore = "core";
inline constexpr std::string_view kSurveillance = "surveillance";
inline constexpr std::string_view kProductivity = "productivity";

inline constexpr std::array kSurveillanceTables{TableName::Camera, TableName::CameraStream, TableName::Zone};
inline constexpr std::array kProductivityTables{TableName::Project, TableName::ProjectTask, TableName::ProjectMember,
                                                TableName::CalendarEvent, TableName::CalendarEventShare};

[[nodiscard]] inline std::span<const TableName> tablesOf(std::string_view moduleId)
{
  if (moduleId == kSurveillance)
    return kSurveillanceTables;
  if (moduleId == kProductivity)
    return kProductivityTables;
  return {};
}

[[nodiscard]] inline std::string_view moduleOf(TableName table)
{
  for (const auto candidate : kSurveillanceTables)
    if (candidate == table)
      return kSurveillance;
  for (const auto candidate : kProductivityTables)
    if (candidate == table)
      return kProductivity;
  return kCore;
}
}
