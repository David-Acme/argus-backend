#include "user-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace user_query;

drogon::Task<std::optional<UserSchema>>
UserRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);

  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front());
}

drogon::Task<UserSchema>
UserRepository::create(const UserCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.name, input.lastName,
                                   userRoleToString(input.role), 1);

  UserSchema schema;
  schema.id = result.insertId();
  schema.name = input.name;
  schema.lastName = input.lastName;
  schema.role = input.role;
  schema.isActive = true;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<UserSchema>
UserRepository::update(int64_t id, const UserUpdateInput& input) const
{
  auto client = DbService::client();
  std::string sql = UPDATE_PREFIX.data();
  std::vector<std::string> args;

  auto addString = [&](std::string_view column,
                       const std::optional<std::string>& value) {
    if (!value)
      return;
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.push_back(*value);
  };

  addString(UPDATE_COL_NAME, input.name);
  addString(UPDATE_COL_LAST_NAME, input.lastName);
  if (input.role) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_ROLE;
    args.push_back(userRoleToString(*input.role));
  }
  if (input.isActive) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_ACTIVE;
    args.push_back(*input.isActive ? "1" : "0");
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "User not found for update";
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
    LOG_WARN << "User not found after update";
    co_return {};
  }

  co_return *updated;
}

drogon::Task<bool> UserRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
UserRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();

  const auto [query, args] =
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(UserSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
UserRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();

  const auto [query, args] =
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM, FIND_DELETED_ALL);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(UserSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> UserRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());

  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>> UserRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());

  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front()).toJson();
}
