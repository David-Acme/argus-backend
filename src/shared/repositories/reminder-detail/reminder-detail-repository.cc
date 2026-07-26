#include "reminder-detail-repository.hxx"
#include <shared/services/sqlite/db-service.hxx>

using namespace reminder_detail_query;

drogon::Task<std::optional<ReminderDetailSchema>>
ReminderDetailRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty()) co_return std::nullopt;
  co_return ReminderDetailSchema(result.front());
}

drogon::Task<std::vector<ReminderDetailSchema>>
ReminderDetailRepository::findByReminder(int64_t reminderId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_REMINDER.data(), reminderId);

  std::vector<ReminderDetailSchema> data;
  for (const auto& row : result)
    data.push_back(ReminderDetailSchema(row));
  co_return data;
}

drogon::Task<ReminderDetailSchema>
ReminderDetailRepository::create(
    const ReminderDetailCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.reminderId,
      input.createdBy ? *input.createdBy : std::optional<int64_t>{},
      input.content,
      reminderDetailStatusToString(input.status), input.filePaths);

  auto created = co_await findById(result.insertId());
  if (!created) {
    LOG_WARN << "ReminderDetail not found after insert";
    co_return {};
  }
  co_return *created;
}

drogon::Task<ReminderDetailSchema>
ReminderDetailRepository::update(
    int64_t id, const ReminderDetailUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(UPDATE.data(), input.content,
                               reminderDetailStatusToString(input.status),
                               input.filePaths, id);

  auto updated = co_await findById(id);
  if (!updated) {
    LOG_WARN << "ReminderDetail not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ReminderDetailRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ReminderDetailRepository::find(const SyncFilter& filter) const
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
    data.push_back(ReminderDetailSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ReminderDetailRepository::findDeleted(const SyncFilter& filter) const
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
    data.push_back(ReminderDetailSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
ReminderDetailRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty()) co_return std::nullopt;
  co_return ReminderDetailSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
ReminderDetailRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty()) co_return std::nullopt;
  co_return ReminderDetailSchema(result.front()).toJson();
}
