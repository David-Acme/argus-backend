#include "user-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace user_query;

namespace
{
sync_query::SyncQueryParts scopedToUser(sync_query::SyncQueryParts parts,
                                        const std::optional<int64_t>& userId)
{
  if (!userId)
    return parts;
  const auto orderBy = parts.query.find(" ORDER BY ");
  parts.query.insert(orderBy, " AND id = ?");
  parts.args.push_back(std::to_string(*userId));
  return parts;
}
} // namespace

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
                                   userRoleToString(input.role), input.lang);

  UserSchema schema;
  schema.id = result.insertId();
  schema.name = input.name;
  schema.lastName = input.lastName;
  schema.role = input.role;
  schema.lang = input.lang;
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

drogon::Task<bool> UserRepository::hasOwner() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(COUNT_OWNERS.data());
  if (result.empty())
    co_return false;
  const auto& row = result.front();
  co_return row[0].as<int64_t>() > 0;
}

drogon::Task<bool> UserRepository::hasAnyUser() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(COUNT_ANY.data());
  co_return !result.empty() && result.front()[0].as<int64_t>() > 0;
}

drogon::Task<bool>
UserRepository::hasOtherActiveOwner(int64_t excludedUserId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      COUNT_OTHER_ACTIVE_OWNERS.data(), excludedUserId);
  co_return !result.empty() && result.front()[0].as<int64_t>() > 0;
}

drogon::Task<std::vector<UserSchema>> UserRepository::findAll() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_ALL.data());
  std::vector<UserSchema> users;
  users.reserve(result.size());
  for (const auto& row : result)
    users.emplace_back(row);
  co_return users;
}

drogon::Task<std::vector<Json::Value>>
UserRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();

  const auto [query, args] = scopedToUser(
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL, FIND_AFTER,
                                 FIND_AFTER_FROM),
      filter.userId);
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
  auto client = DbService::readOnlyClient();

  const auto [query, args] = scopedToUser(
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM,
                                 FIND_DELETED_ALL, FIND_DELETED_AFTER,
                                 FIND_DELETED_AFTER_FROM),
      filter.userId);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(UserSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> UserRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();

  if (filter.userId) {
    const int64_t userId = *filter.userId;
    const auto result =
        co_await client->execSqlCoro(FIND_LAST_FOR_USER.data(), userId);
    if (result.empty())
      co_return std::nullopt;
    co_return UserSchema(result.front()).toJson();
  }

  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>> UserRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();

  if (filter.userId) {
    const int64_t userId = *filter.userId;
    const auto result =
        co_await client->execSqlCoro(FIND_LAST_DELETED_FOR_USER.data(), userId);
    if (result.empty())
      co_return std::nullopt;
    co_return UserSchema(result.front()).toJson();
  }

  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front()).toJson();
}
