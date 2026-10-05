#include "environment-repository.hxx"

#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace environment_query;

namespace
{
GuardEnvironment fromRow(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<int64_t>(),
          .name = row["name"].as<std::string>(),
          .kind = environmentKindFromString(row["kind"].as<std::string>())
                      .value_or(EnvironmentKind::Home),
          .isDefault = row["is_default"].as<int>() != 0,
          .mode = guardModeFromString(row["mode"].as<std::string>()),
          .modeUpdatedAt = row["mode_updated_at"].as<int64_t>(),
          .scheduleEnabled = row["schedule_enabled"].as<int>() != 0,
          .asleep = row["asleep_hours"].as<std::string>(),
          .open = row["open_hours"].as<std::string>(),
          .staffed = row["staffed_hours"].as<std::string>(),
          .closedMode = guardModeFromString(row["closed_mode"].as<std::string>()),
          .digestHour = row["digest_hour"].as<int>(),
          .quietPolicy =
              quietPolicyFromString(row["quiet_policy"].as<std::string>())
                  .value_or(QuietPolicy::Inherit),
          .quietStartHour = row["quiet_start_hour"].as<int>(),
          .quietEndHour = row["quiet_end_hour"].as<int>(),
          .lanPresence = row["lan_presence"].as<int>() != 0,
          .createdAt = row["created_at"].as<int64_t>(),
          .updatedAt = row["updated_at"].as<int64_t>()};
}

std::string closedModeName(GuardMode mode)
{
  return mode == GuardMode::Armed ? "armed" : "away";
}
}

drogon::Task<std::vector<GuardEnvironment>> EnvironmentRepository::list() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(LIST_ENVIRONMENTS));
  std::vector<GuardEnvironment> environments;
  environments.reserve(rows.size());
  for (const auto& row : rows)
    environments.push_back(fromRow(row));
  co_return environments;
}

drogon::Task<std::optional<GuardEnvironment>>
EnvironmentRepository::find(int64_t id) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(FIND_ENVIRONMENT), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return fromRow(rows.front());
}

drogon::Task<std::optional<GuardEnvironmentScope>>
EnvironmentRepository::forCamera(int64_t cameraId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(FOR_CAMERA), cameraId);
  if (rows.empty())
    co_return std::nullopt;
  co_return GuardEnvironmentScope{
      .environment = fromRow(rows.front()),
      .several = rows.front()["total"].as<int64_t>() > 1};
}

drogon::Task<int64_t> EnvironmentRepository::count() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(COUNT_ENVIRONMENTS));
  co_return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

drogon::Task<int64_t> EnvironmentRepository::defaultId() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(DEFAULT_ID));
  co_return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

drogon::Task<GuardEnvironment>
EnvironmentRepository::create(const EnvironmentCreateInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(INSERT_ENVIRONMENT), input.name,
      environmentKindToString(input.kind), guardModeToString(input.mode),
      input.at, input.scheduleEnabled ? 1 : 0, input.asleep, input.open,
      input.staffed, closedModeName(input.closedMode), input.digestHour,
      quietPolicyToString(input.quietPolicy), input.quietStartHour,
      input.quietEndHour, input.lanPresence ? 1 : 0, input.at, input.at);
  if (rows.empty())
    throw std::runtime_error("environment insert returned no row");
  const auto created = co_await find(rows.front()["id"].as<int64_t>());
  if (!created)
    throw std::runtime_error("environment insert lost its row");
  co_return *created;
}

drogon::Task<std::optional<GuardEnvironment>>
EnvironmentRepository::update(const EnvironmentUpdateInput& input) const
{
  std::string sql(UPDATE_ENVIRONMENT_PREFIX);
  std::vector<std::string> args{std::to_string(input.at)};
  const auto addField = [&sql, &args](std::string_view column,
                                      std::string value) {
    sql += column;
    args.push_back(std::move(value));
  };
  if (input.name)
    addField(UPDATE_COL_NAME, *input.name);
  if (input.kind)
    addField(UPDATE_COL_KIND, environmentKindToString(*input.kind));
  if (input.scheduleEnabled)
    addField(UPDATE_COL_SCHEDULE_ENABLED, *input.scheduleEnabled ? "1" : "0");
  if (input.asleep)
    addField(UPDATE_COL_ASLEEP, *input.asleep);
  if (input.open)
    addField(UPDATE_COL_OPEN, *input.open);
  if (input.staffed)
    addField(UPDATE_COL_STAFFED, *input.staffed);
  if (input.closedMode)
    addField(UPDATE_COL_CLOSED_MODE, closedModeName(*input.closedMode));
  if (input.digestHour)
    addField(UPDATE_COL_DIGEST_HOUR, std::to_string(*input.digestHour));
  if (input.quietPolicy)
    addField(UPDATE_COL_QUIET_POLICY, quietPolicyToString(*input.quietPolicy));
  if (input.quietStartHour)
    addField(UPDATE_COL_QUIET_START, std::to_string(*input.quietStartHour));
  if (input.quietEndHour)
    addField(UPDATE_COL_QUIET_END, std::to_string(*input.quietEndHour));
  if (input.lanPresence)
    addField(UPDATE_COL_LAN_PRESENCE, *input.lanPresence ? "1" : "0");
  sql += UPDATE_ENVIRONMENT_SUFFIX;
  args.push_back(std::to_string(input.id));
  const auto& argsRef = args;
  const auto result = co_await DbService::client()->execSqlCoro(sql, argsRef);
  if (result.affectedRows() == 0)
    co_return std::nullopt;
  co_return co_await find(input.id);
}

drogon::Task<int64_t>
EnvironmentRepository::setMode(const EnvironmentModeInput& input) const
{
  const auto client = DbService::client();
  const std::string mode = guardModeToString(input.mode);
  if (input.environmentId) {
    const auto result = co_await client->execSqlCoro(
        std::string(SET_MODE_ONE), mode, input.at, *input.environmentId);
    co_return static_cast<int64_t>(result.affectedRows());
  }
  const auto result =
      co_await client->execSqlCoro(std::string(SET_MODE_ALL), mode, input.at);
  co_return static_cast<int64_t>(result.affectedRows());
}

drogon::Task<EnvironmentRemoval>
EnvironmentRepository::remove(const EnvironmentRemoveInput& input) const
{
  const auto target = co_await find(input.id);
  if (!target)
    co_return EnvironmentRemoval::NotFound;
  if (target->isDefault)
    co_return EnvironmentRemoval::IsDefault;
  const int64_t fallback = co_await defaultId();
  std::shared_ptr<drogon::orm::Transaction> transaction =
      co_await db_transaction::begin(DbService::client());
  try {
    co_await transaction->execSqlCoro(std::string(MOVE_CAMERAS), fallback,
                                      input.at, input.id);
    co_await transaction->execSqlCoro(std::string(RETIRE_GUESTS), input.at,
                                      input.id);
    const auto deleted = co_await transaction->execSqlCoro(
        std::string(DELETE_ENVIRONMENT), input.id);
    if (deleted.affectedRows() == 0) {
      transaction->rollback();
      co_return EnvironmentRemoval::NotFound;
    }
  }
  catch (const std::exception& error) {
    transaction->rollback();
    LOG_WARN << "Guard environment removal failed: " << error.what();
    throw;
  }
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw std::runtime_error("environment removal did not commit");
  co_return EnvironmentRemoval::Removed;
}

drogon::Task<std::vector<CameraAssignment>>
EnvironmentRepository::cameraAssignments() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(CAMERA_ASSIGNMENTS));
  std::vector<CameraAssignment> assignments;
  assignments.reserve(rows.size());
  for (const auto& row : rows)
    assignments.push_back({.cameraId = row["camera_id"].as<int64_t>(),
                           .environmentId = row["environment_id"].as<int64_t>()});
  co_return assignments;
}
