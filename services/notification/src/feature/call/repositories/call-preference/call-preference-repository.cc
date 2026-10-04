#include "call-preference-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

#include <stdexcept>
#include <string>

using namespace call_preference_query;

namespace
{
struct Assignment
{
  std::string_view column;
  std::string value;
};

std::string mutedJson(const std::vector<int64_t>& ids)
{
  Json::Value array(Json::arrayValue);
  for (const int64_t id : ids)
    array.append(static_cast<Json::Int64>(id));
  return json_util::toString(array);
}

std::vector<Assignment> assignments(const CallPreferenceUpdateInput& input)
{
  std::vector<Assignment> list;
  const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
  if (input.enabled)
    list.push_back({.column = UPDATE_COL_ENABLED, .value = flag(*input.enabled)});
  if (input.guardCritical)
    list.push_back({.column = UPDATE_COL_GUARD_CRITICAL,
                    .value = callModeToString(*input.guardCritical)});
  if (input.guardIntruder)
    list.push_back({.column = UPDATE_COL_GUARD_INTRUDER,
                    .value = callModeToString(*input.guardIntruder)});
  if (input.guardEscalation)
    list.push_back({.column = UPDATE_COL_GUARD_ESCALATION,
                    .value = callModeToString(*input.guardEscalation)});
  if (input.guardArrival)
    list.push_back({.column = UPDATE_COL_GUARD_ARRIVAL,
                    .value = callModeToString(*input.guardArrival)});
  if (input.agenda)
    list.push_back(
        {.column = UPDATE_COL_AGENDA, .value = callModeToString(*input.agenda)});
  if (input.assistant)
    list.push_back({.column = UPDATE_COL_ASSISTANT,
                    .value = callModeToString(*input.assistant)});
  if (input.quietStartHour)
    list.push_back({.column = UPDATE_COL_QUIET_START,
                    .value = std::to_string(*input.quietStartHour)});
  if (input.quietEndHour)
    list.push_back({.column = UPDATE_COL_QUIET_END,
                    .value = std::to_string(*input.quietEndHour)});
  if (input.dndUntil)
    list.push_back({.column = UPDATE_COL_DND_UNTIL,
                    .value = std::to_string(*input.dndUntil)});
  if (input.criticalBypass)
    list.push_back({.column = UPDATE_COL_CRITICAL_BYPASS,
                    .value = flag(*input.criticalBypass)});
  if (input.mutedEnvironmentIds)
    list.push_back({.column = UPDATE_COL_MUTED,
                    .value = mutedJson(*input.mutedEnvironmentIds)});
  list.push_back({.column = UPDATE_COL_UPDATED_AT,
                  .value = std::to_string(input.updatedAt)});
  return list;
}
}

drogon::Task<std::optional<CallPreferenceSchema>>
CallPreferenceRepository::find(int64_t userId) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(FIND), userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return CallPreferenceSchema::fromRow(rows.front());
}

drogon::Task<std::unordered_map<int64_t, CallPreferenceSchema>>
CallPreferenceRepository::findMany(const std::vector<int64_t>& userIds) const
{
  std::unordered_map<int64_t, CallPreferenceSchema> found;
  if (userIds.empty())
    co_return found;
  std::string sql(FIND_MANY_PREFIX);
  for (std::size_t index = 0; index < userIds.size(); ++index)
    sql += index == 0 ? "?" : ", ?";
  sql += ')';
  const auto client = DbService::client();
  std::vector<std::string> args;
  args.reserve(userIds.size());
  for (const int64_t userId : userIds)
    args.push_back(std::to_string(userId));
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(sql, argsRef);
  for (const auto& row : rows) {
    auto preference = CallPreferenceSchema::fromRow(row);
    found.emplace(preference.userId, std::move(preference));
  }
  co_return found;
}

drogon::Task<std::vector<CallPreferenceSchema>>
CallPreferenceRepository::findArrivalSubscribers() const
{
  const auto client = DbService::client();
  const auto rows =
      co_await client->execSqlCoro(std::string(FIND_ARRIVAL_SUBSCRIBERS));
  std::vector<CallPreferenceSchema> found;
  found.reserve(rows.size());
  for (const auto& row : rows)
    found.push_back(CallPreferenceSchema::fromRow(row));
  co_return found;
}

drogon::Task<CallPreferenceSchema>
CallPreferenceRepository::update(const CallPreferenceUpdateInput& input) const
{
  const auto client = DbService::client();
  co_await client->execSqlCoro(std::string(INSERT_DEFAULTS), input.userId,
                               input.updatedAt);
  const auto list = assignments(input);
  std::string sql(UPDATE_PREFIX);
  for (std::size_t index = 0; index < list.size(); ++index) {
    if (index > 0)
      sql += ", ";
    sql += list[index].column;
  }
  sql += UPDATE_SUFFIX;
  std::vector<std::string> values;
  values.reserve(list.size() + 1);
  for (const auto& assignment : list)
    values.push_back(assignment.value);
  values.push_back(std::to_string(input.userId));
  const auto& valuesRef = values;
  co_await client->execSqlCoro(sql, valuesRef);
  const auto rows = co_await client->execSqlCoro(std::string(FIND), input.userId);
  if (rows.empty())
    throw std::runtime_error("call preference vanished after update");
  co_return CallPreferenceSchema::fromRow(rows.front());
}
