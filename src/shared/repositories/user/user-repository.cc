#include "user-repository.hxx"
#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace user_query;

drogon::Task<std::optional<UserSchema>>
UserRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(
FIND_BY_ID.data(), id);

  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front());
}

drogon::Task<UserSchema>
UserRepository::create(const UserCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      
INSERT.data(), input.featureHubId, input.name,
      input.lastName, userRoleToString(input.role), 1);

  UserSchema schema;
  schema.id = result.insertId();
  schema.featureHubId = input.featureHubId;
  schema.name = input.name;
  schema.lastName = input.lastName;
  schema.role = input.role;
  schema.isActive = true;
  schema.createdAt = result.insertId() ? std::time(nullptr) : 0;
  co_return schema;
}

drogon::Task<UserSchema>
UserRepository::update(int64_t id, const UserUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
UPDATE.data(), input.name,
                               input.lastName,
                               userRoleToString(input.role),
                               input.isActive ? 1 : 0, id);

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
  const auto result =
      co_await client->execSqlCoro(
REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
UserRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();

  auto result = [&]() -> drogon::Task<drogon::orm::Result> {
    if (filter.startTime && filter.endTime) {
      co_return co_await client->execSqlCoro(
          
FIND.data(), *filter.startTime, *filter.endTime);
    }
    if (filter.startTime) {
      co_return co_await client->execSqlCoro(
          
FIND_FROM.data(), *filter.startTime);
    }
    co_return co_await client->execSqlCoro(FIND_ALL.data());
  }();

  std::vector<Json::Value> data;
  for (const auto& row : co_await result)
    data.push_back(UserSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
UserRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();

  auto result = [&]() -> drogon::Task<drogon::orm::Result> {
    if (filter.startTime && filter.endTime) {
      co_return co_await client->execSqlCoro(
          
FIND_DELETED.data(), *filter.startTime,
          *filter.endTime);
    }
    if (filter.startTime) {
      co_return co_await client->execSqlCoro(
          
FIND_DELETED_FROM.data(), *filter.startTime);
    }
    co_return co_await client->execSqlCoro(FIND_DELETED_ALL.data());
  }();

  std::vector<Json::Value> data;
  for (const auto& row : co_await result)
    data.push_back(UserSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
UserRepository::findLast() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(
FIND_LAST.data());

  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
UserRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(
FIND_LAST_DELETED.data());

  if (result.empty())
    co_return std::nullopt;

  co_return UserSchema(result.front()).toJson();
}
