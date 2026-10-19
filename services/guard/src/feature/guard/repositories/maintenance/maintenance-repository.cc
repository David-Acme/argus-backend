#include "maintenance-repository.hxx"

#include <drogon/orm/Result.h>
#include <sqlite/db-service.hxx>

#include <string>

using namespace maintenance_query;

namespace
{
MaintenanceWindow windowOf(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<int64_t>(),
          .environmentId = row["environment_id"].as<int64_t>(),
          .cameraIds = maintenance_schema::idsFromJson(row["camera_ids"].as<std::string>()),
          .declaredBy = row["declared_by"].as<int64_t>(),
          .declaredRole = parseUserRole(row["declared_role"].as<std::string>()).value_or(UserRole::Unknown),
          .declaredName = row["declared_name"].as<std::string>(),
          .source = maintenanceSourceFromString(row["source"].as<std::string>()).value_or(MaintenanceSource::App),
          .duress = row["duress"].as<int64_t>() != 0,
          .startedAt = row["started_at"].as<int64_t>(),
          .endsAt = row["ends_at"].as<int64_t>(),
          .endedAt = row["ended_at"].as<int64_t>(),
          .endReason = maintenanceEndReasonFromString(row["end_reason"].as<std::string>())
                           .value_or(MaintenanceEndReason::None),
          .viewChecks = maintenance_schema::viewChecksFromJson(row["view_checks"].as<std::string>())};
}

TamperAsk askOf(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<int64_t>(),
          .cameraId = row["camera_id"].as<int64_t>(),
          .environmentId = row["environment_id"].as<int64_t>(),
          .onsetAt = row["onset_at"].as<int64_t>(),
          .askedAt = row["asked_at"].as<int64_t>(),
          .askedUsers = maintenance_schema::idsFromJson(row["asked_users"].as<std::string>()),
          .answer = tamperAskAnswerFromString(row["answer"].as<std::string>()).value_or(TamperAskAnswer::Pending),
          .answeredBy = row["answered_by"].as<int64_t>(),
          .answeredAt = row["answered_at"].as<int64_t>()};
}

std::vector<MaintenanceWindow> windowsOf(const drogon::orm::Result& rows)
{
  std::vector<MaintenanceWindow> windows;
  windows.reserve(rows.size());
  for (const auto& row : rows)
    windows.push_back(windowOf(row));
  return windows;
}
}

drogon::Task<MaintenanceWindow> MaintenanceRepository::create(const MaintenanceCreateInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(INSERT_WINDOW), input.environmentId, maintenance_schema::idsToJson(input.cameraIds),
      input.declaredBy, userRoleToString(input.declaredRole), input.declaredName,
      maintenanceSourceToString(input.source), static_cast<int64_t>(input.duress ? 1 : 0), input.startedAt,
      input.endsAt);
  co_return MaintenanceWindow{.id = static_cast<int64_t>(result.insertId()),
                              .environmentId = input.environmentId,
                              .cameraIds = input.cameraIds,
                              .declaredBy = input.declaredBy,
                              .declaredRole = input.declaredRole,
                              .declaredName = input.declaredName,
                              .source = input.source,
                              .duress = input.duress,
                              .startedAt = input.startedAt,
                              .endsAt = input.endsAt,
                              .endedAt = 0,
                              .endReason = MaintenanceEndReason::None,
                              .viewChecks = {}};
}

drogon::Task<std::optional<MaintenanceWindow>> MaintenanceRepository::findById(int64_t id) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_WINDOW), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return windowOf(rows[0]);
}

drogon::Task<std::vector<MaintenanceWindow>> MaintenanceRepository::open() const
{
  co_return windowsOf(co_await DbService::client()->execSqlCoro(std::string(LIST_OPEN)));
}

drogon::Task<std::vector<MaintenanceWindow>> MaintenanceRepository::recent(int64_t since) const
{
  co_return windowsOf(co_await DbService::client()->execSqlCoro(std::string(LIST_RECENT), since));
}

drogon::Task<bool> MaintenanceRepository::end(const MaintenanceEndInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(END_WINDOW), input.at, maintenanceEndReasonToString(input.reason), input.id);
  co_return result.affectedRows() > 0;
}

drogon::Task<void> MaintenanceRepository::setViewChecks(const MaintenanceViewChecksInput& input) const
{
  co_await DbService::client()->execSqlCoro(std::string(SET_VIEW_CHECKS),
                                            maintenance_schema::viewChecksToJson(input.checks), input.id);
}

drogon::Task<bool> MaintenanceRepository::recentDuress(const DuressLookupInput& input) const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(RECENT_DURESS), input.userId, input.since);
  co_return !rows.empty() && rows[0]["total"].as<int64_t>() > 0;
}

drogon::Task<TamperAsk> MaintenanceRepository::createAsk(const TamperAskCreateInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(INSERT_ASK), input.cameraId, input.environmentId, input.onsetAt, input.askedAt,
      maintenance_schema::idsToJson(input.askedUsers));
  co_return TamperAsk{.id = static_cast<int64_t>(result.insertId()),
                      .cameraId = input.cameraId,
                      .environmentId = input.environmentId,
                      .onsetAt = input.onsetAt,
                      .askedAt = input.askedAt,
                      .askedUsers = input.askedUsers,
                      .answer = TamperAskAnswer::Pending,
                      .answeredBy = 0,
                      .answeredAt = 0};
}

drogon::Task<std::optional<TamperAsk>> MaintenanceRepository::findAsk(int64_t id) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_ASK), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return askOf(rows[0]);
}

drogon::Task<std::optional<TamperAsk>>
MaintenanceRepository::askForOnset(const TamperAskOnsetInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_ASK_FOR_ONSET),
                                                              input.cameraId, input.onsetAt);
  if (rows.empty())
    co_return std::nullopt;
  co_return askOf(rows[0]);
}

drogon::Task<std::vector<TamperAsk>> MaintenanceRepository::pendingAsks() const
{
  std::vector<TamperAsk> asks;
  for (const auto& row : co_await DbService::client()->execSqlCoro(std::string(PENDING_ASKS)))
    asks.push_back(askOf(row));
  co_return asks;
}

drogon::Task<bool> MaintenanceRepository::answerAsk(const TamperAskAnswerInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(ANSWER_ASK), tamperAskAnswerToString(input.answer), input.answeredBy, input.at, input.id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::optional<IncidentRef>> MaintenanceRepository::incident(int64_t id) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_INCIDENT), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return IncidentRef{.id = rows[0]["id"].as<int64_t>(),
                        .cameraId = rows[0]["camera_id"].as<int64_t>(),
                        .rule = rows[0]["rule"].as<std::string>()};
}
