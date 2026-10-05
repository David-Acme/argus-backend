#include "pending-object-delete-repository.hxx"

#include <json/value.h>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

using namespace pending_object_delete_query;

drogon::Task<void>
PendingObjectDeleteRepository::enqueue(const PendingObjectEnqueueInput& input) const
{
  if (input.objectKeys.empty())
    co_return;
  Json::Value keys(Json::arrayValue);
  for (const auto& key : input.objectKeys)
    keys.append(key);
  const auto pooled = DbService::identityClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  co_await client->execSqlCoro(std::string(ENQUEUE), json_util::toString(keys));
}

drogon::Task<std::vector<PendingObjectDelete>>
PendingObjectDeleteRepository::findDue(const PendingObjectDueInput& input) const
{
  const auto rows = co_await DbService::identityClient()->execSqlCoro(
      std::string(FIND_DUE), input.now, input.limit);
  std::vector<PendingObjectDelete> due;
  due.reserve(rows.size());
  for (const auto& row : rows)
    due.push_back({.id = row["id"].as<int64_t>(),
                   .objectKey = row["object_key"].as<std::string>(),
                   .attempts = row["attempts"].as<int64_t>()});
  co_return due;
}

drogon::Task<void> PendingObjectDeleteRepository::remove(int64_t id) const
{
  co_await DbService::identityClient()->execSqlCoro(std::string(REMOVE), id);
}

drogon::Task<void>
PendingObjectDeleteRepository::postpone(const PendingObjectPostponeInput& input) const
{
  co_await DbService::identityClient()->execSqlCoro(std::string(POSTPONE),
                                                    input.nextAttemptAt, input.id);
}

drogon::Task<int64_t> PendingObjectDeleteRepository::count() const
{
  const auto rows =
      co_await DbService::identityClient()->execSqlCoro(std::string(COUNT));
  co_return rows.empty() ? 0 : rows.front()["pending"].as<int64_t>();
}
