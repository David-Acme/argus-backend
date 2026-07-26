#include "reminder-repository.hxx"
#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace reminder_query;

drogon::Task<std::optional<ReminderSchema>>
ReminderRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty()) co_return std::nullopt;
  co_return ReminderSchema(result.front());
}

drogon::Task<std::vector<ReminderSchema>>
ReminderRepository::findByTargetUser(int64_t targetUserId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_TARGET.data(), targetUserId);

  std::vector<ReminderSchema> data;
  for (const auto& row : result)
    data.push_back(ReminderSchema(row));
  co_return data;
}

drogon::Task<ReminderSchema>
ReminderRepository::create(const ReminderCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(),
      input.createdBy ? *input.createdBy : std::optional<int64_t>{},
      input.targetUserId, input.title, input.description,
      input.scheduledAt,
      input.recurrenceRule ? *input.recurrenceRule
                          : std::optional<std::string>{});

  ReminderSchema schema;
  schema.id = result.insertId();
  schema.createdBy = input.createdBy;
  schema.targetUserId = input.targetUserId;
  schema.title = input.title;
  schema.description = input.description;
  schema.scheduledAt = input.scheduledAt;
  schema.recurrenceRule = input.recurrenceRule;
  schema.isCompleted = false;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<ReminderSchema>
ReminderRepository::update(int64_t id,
                           const ReminderUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
      UPDATE.data(), input.title, input.description, input.scheduledAt,
      input.recurrenceRule ? *input.recurrenceRule
                          : std::optional<std::string>{},
      input.isCompleted ? 1 : 0,
      input.completedAt ? *input.completedAt
                        : std::optional<int64_t>{},
      id);

  auto updated = co_await findById(id);
  if (!updated) {
    LOG_WARN << "Reminder not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ReminderRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ReminderRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();
  auto result = [&]() -> drogon::Task<drogon::orm::Result> {
    if (filter.startTime && filter.endTime)
      co_return co_await client->execSqlCoro(FIND.data(), *filter.startTime,
                                             *filter.endTime);
    if (filter.startTime)
      co_return co_await client->execSqlCoro(FIND_FROM.data(),
                                             *filter.startTime);
    co_return co_await client->execSqlCoro(FIND_ALL.data());
  }();

  std::vector<Json::Value> data;
  for (const auto& row : co_await result)
    data.push_back(ReminderSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ReminderRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();
  auto result = [&]() -> drogon::Task<drogon::orm::Result> {
    if (filter.startTime && filter.endTime)
      co_return co_await client->execSqlCoro(
          FIND_DELETED.data(), *filter.startTime, *filter.endTime);
    if (filter.startTime)
      co_return co_await client->execSqlCoro(FIND_DELETED_FROM.data(),
                                             *filter.startTime);
    co_return co_await client->execSqlCoro(FIND_DELETED_ALL.data());
  }();

  std::vector<Json::Value> data;
  for (const auto& row : co_await result)
    data.push_back(ReminderSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
ReminderRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty()) co_return std::nullopt;
  co_return ReminderSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
ReminderRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty()) co_return std::nullopt;
  co_return ReminderSchema(result.front()).toJson();
}
