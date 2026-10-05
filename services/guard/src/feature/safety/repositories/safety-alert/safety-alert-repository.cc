#include "safety-alert-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace safety_alert_query;

drogon::Task<int64_t> SafetyAlertRepository::insert(const SafetyAlertInsertInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(INSERT_ALERT), safetyAlertKindToString(input.kind), input.userId,
      input.environmentId, input.now, input.actorName);
  co_return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

drogon::Task<std::optional<SafetyAlertRecent>>
SafetyAlertRepository::recent(const SafetyAlertRecentInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(RECENT_ALERT), safetyAlertKindToString(input.kind), input.userId,
      input.since);
  if (rows.empty())
    co_return std::nullopt;
  co_return SafetyAlertRecent{.id = rows.front()["id"].as<int64_t>(),
                              .notified = rows.front()["notified_at"].as<int64_t>() > 0};
}

drogon::Task<int64_t> SafetyAlertRepository::countSince(const SafetyAlertRecentInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(COUNT_SINCE), safetyAlertKindToString(input.kind), input.userId, input.since);
  co_return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

drogon::Task<std::vector<SafetyAlertRow>> SafetyAlertRepository::pending() const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(PENDING_ALERTS));
  std::vector<SafetyAlertRow> alerts;
  alerts.reserve(rows.size());
  for (const auto& row : rows) {
    const auto kind = safetyAlertKindFromString(row["kind"].as<std::string>());
    if (!kind)
      continue;
    alerts.push_back({.id = row["id"].as<int64_t>(),
                      .kind = *kind,
                      .userId = row["user_id"].as<int64_t>(),
                      .environmentId = row["environment_id"].as<int64_t>(),
                      .createdAt = row["created_at"].as<int64_t>(),
                      .sequence = row["notify_sequence"].as<int64_t>(),
                      .escalatedAt = row["escalated_at"].as<int64_t>(),
                      .actorName = row["actor_name"].as<std::string>()});
  }
  co_return alerts;
}

drogon::Task<bool> SafetyAlertRepository::advanceSequence(const SafetyAlertSequence& current) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(ADVANCE_SEQUENCE), current.id, current.sequence);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> SafetyAlertRepository::markEscalated(int64_t id, int64_t now) const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(MARK_ESCALATED), now, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<void> SafetyAlertRepository::markNotified(int64_t id, int64_t now) const
{
  co_await DbService::client()->execSqlCoro(std::string(MARK_NOTIFIED), now, id);
}

drogon::Task<int64_t> SafetyAlertRepository::purgeBefore(int64_t createdBefore) const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(PURGE_BEFORE), createdBefore);
  co_return static_cast<int64_t>(result.affectedRows());
}
