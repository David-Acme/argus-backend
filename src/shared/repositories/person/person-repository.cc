#include "person-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace person_query;

drogon::Task<std::optional<PersonSchema>>
PersonRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return PersonSchema(result.front());
}

drogon::Task<std::vector<PersonSchema>>
PersonRepository::findByUser(int64_t userId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_USER.data(), userId);

  std::vector<PersonSchema> data;
  for (const auto& row : result)
    data.push_back(PersonSchema(row));
  co_return data;
}

drogon::Task<PersonSchema>
PersonRepository::create(const PersonCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(),
                                   input.userId ? *input.userId
                                                : std::optional<int64_t>{},
                                   input.name, input.alias, input.observation);

  PersonSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.name = input.name;
  schema.alias = input.alias;
  schema.observation = input.observation;
  schema.firstSeenAt = std::time(nullptr);
  schema.lastSeenAt = std::time(nullptr);
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<PersonSchema>
PersonRepository::update(int64_t id, const PersonUpdateInput& input) const
{
  auto client = DbService::client();
  std::string sql = UPDATE_PREFIX.data();
  std::vector<std::string> args;

  auto addField = [&](std::string_view column,
                      const std::optional<std::string>& value) {
    if (!value)
      return;
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.push_back(*value);
  };

  addField(UPDATE_COL_NAME, input.name);
  addField(UPDATE_COL_ALIAS, input.alias);
  addField(UPDATE_COL_OBSERVATION, input.observation);
  if (input.lastSeenAt) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_LAST_SEEN;
    args.push_back(std::to_string(*input.lastSeenAt));
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "Person not found for update";
      co_return {};
    }
    co_return *existing;
  }

  sql += UPDATE_SUFFIX.data();
  args.push_back(std::to_string(id));
  const auto& argsRef = args;
  co_await client->execSqlCoro(sql, argsRef);

  auto updated = co_await findById(id);
  if (!updated) {
    LOG_WARN << "Person not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> PersonRepository::linkUser(int64_t id, int64_t userId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(UPDATE_USER.data(), userId, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> PersonRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
PersonRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto [query, args] =
      sync_query::buildSyncQuery(filter, SYNC_FIND, SYNC_FIND_FROM,
                                 SYNC_FIND_ALL, SYNC_FIND_AFTER,
                                 SYNC_FIND_AFTER_FROM);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(PersonSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
PersonRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto [query, args] = sync_query::buildSyncQuery(
      filter, SYNC_FIND_DELETED, SYNC_FIND_DELETED_FROM,
      SYNC_FIND_DELETED_ALL, SYNC_FIND_DELETED_AFTER,
      SYNC_FIND_DELETED_AFTER_FROM);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(PersonSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> PersonRepository::findLast(const SyncFilter&) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(SYNC_FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return PersonSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>> PersonRepository::findLastDeleted(const SyncFilter&) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(SYNC_FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return PersonSchema(result.front()).toJson();
}
