#pragma once

#include <algorithm>
#include <array>
#include <json/value.h>
#include <optional>
#include <string>
#include <string_view>
#include <sync/table-name.hxx>
#include <sync/user-action-event.hxx>

namespace action_journal
{
inline constexpr std::string_view kTitleField = "title";

struct JournalCeiling
{
  TableName table;
  bool title;
};

inline constexpr std::array<JournalCeiling, 7> kPrivateTables = {{
    {.table = TableName::Reminder, .title = false},
    {.table = TableName::ReminderDetail, .title = false},
    {.table = TableName::Project, .title = true},
    {.table = TableName::ProjectTask, .title = true},
    {.table = TableName::ProjectMember, .title = false},
    {.table = TableName::CalendarEvent, .title = true},
    {.table = TableName::CalendarEventShare, .title = false},
}};

inline std::optional<JournalCeiling> ceilingOf(TableName table)
{
  const auto* found = std::ranges::find(kPrivateTables, table, &JournalCeiling::table);
  if (found == kPrivateTables.end())
    return std::nullopt;
  return *found;
}

inline Json::Value journalData(TableName table, const std::optional<std::string>& sharedTitle)
{
  const auto ceiling = ceilingOf(table);
  if (!ceiling || !ceiling->title || !sharedTitle || sharedTitle->empty())
    return {};
  Json::Value data(Json::objectValue);
  data[std::string(kTitleField)] = *sharedTitle;
  return data;
}

inline void restrict(UserActionEvent& event)
{
  if (!event.subject.empty())
    return;
  const auto ceiling = ceilingOf(event.tableName);
  if (!ceiling)
    return;
  std::optional<std::string> kept;
  if (event.newData.isObject()) {
    const Json::Value& title = event.newData[std::string(kTitleField)];
    if (title.isString())
      kept = title.asString();
  }
  event.oldData = Json::Value();
  event.newData = journalData(event.tableName, kept);
}
}
