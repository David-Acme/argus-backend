#include "project-member-repository.hxx"

#include <sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

using namespace project_member_query;

drogon::Task<std::optional<ProjectMemberSchema>>
ProjectMemberRepository::findById(int64_t id, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::productivityClient();
  auto* effective = client ? client : pooled.get();
  const auto result = co_await effective->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectMemberSchema(result.front());
}

drogon::Task<std::vector<ProjectMemberSchema>>
ProjectMemberRepository::findByParent(int64_t parentId) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(FIND_BY_PARENT.data(), parentId);
  std::vector<ProjectMemberSchema> rows;
  for (const auto& row : result)
    rows.emplace_back(row);
  co_return rows;
}

drogon::Task<std::optional<ShareAccess>>
ProjectMemberRepository::findAccess(const ProjectMemberLookupInput& input) const
{
  const auto pooled = DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(FIND_ACCESS.data(),
                                                   input.parentId, input.userId);
  if (result.empty())
    co_return std::nullopt;
  co_return shareAccessFromString(result.front()["access"].as<std::string>());
}

drogon::Task<std::vector<int64_t>>
ProjectMemberRepository::memberIds(int64_t parentId,
                                   drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::productivityClient();
  auto* effective = client ? client : pooled.get();
  const auto result =
      co_await effective->execSqlCoro(FIND_MEMBER_IDS.data(), parentId);
  std::vector<int64_t> ids;
  ids.reserve(result.size());
  for (const auto& row : result)
    ids.push_back(static_cast<int64_t>(row["user_id"].as<long long>()));
  co_return ids;
}

drogon::Task<std::optional<ProjectMemberSchema>>
ProjectMemberRepository::findExisting(const ProjectMemberLookupInput& input) const
{
  const auto pooled = DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      FIND_EXISTING.data(), input.parentId, input.userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectMemberSchema(result.front());
}

drogon::Task<ProjectMemberSchema>
ProjectMemberRepository::create(const ProjectMemberCreateInput& input) const
{
  const auto pooled = DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.projectId, input.userId,
      shareAccessToString(input.access));
  const auto row = co_await findById(static_cast<int64_t>(result.insertId()), client);
  if (!row) {
    LOG_WARN << "Membership row vanished right after insert";
    co_return ProjectMemberSchema{};
  }
  co_return *row;
}

drogon::Task<ProjectMemberSchema>
ProjectMemberRepository::updateAccess(const ProjectMemberUpdateInput& input) const
{
  const auto pooled = DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(UPDATE_ACCESS.data(),
                               shareAccessToString(input.access), input.id);
  const auto row = co_await findById(input.id, client);
  if (!row)
    co_return ProjectMemberSchema{};
  co_return *row;
}

drogon::Task<bool> ProjectMemberRepository::remove(int64_t id,
                                                   drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::productivityClient();
  auto* effective = client ? client : pooled.get();
  const auto result = co_await effective->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ProjectMemberRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto [query, args] = sync_query::withUser(
                                   {.parts = sync_query::buildSyncQuery({.filter = filter,
                                                                        .queryBoth = FIND,
                                                                        .queryFrom = FIND_FROM,
                                                                        .queryAll = FIND_ALL,
                                                                        .queryAfterBoth = FIND_AFTER,
                                                                        .queryAfterFrom = FIND_AFTER_FROM}),
                                    .userId = filter.userId,
                                    .placeholders = OWNERSHIP_PLACEHOLDERS});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ProjectMemberSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ProjectMemberRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto [query, args] = sync_query::withUser(
                                   {.parts = sync_query::buildSyncQuery({.filter = filter,
                                                                        .queryBoth = FIND_DELETED,
                                                                        .queryFrom = FIND_DELETED_FROM,
                                                                        .queryAll = FIND_DELETED_ALL,
                                                                        .queryAfterBoth = FIND_DELETED_AFTER,
                                                                        .queryAfterFrom = FIND_DELETED_AFTER_FROM}),
                                    .userId = filter.userId,
                                    .placeholders = OWNERSHIP_PLACEHOLDERS});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ProjectMemberSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
ProjectMemberRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectMemberSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
ProjectMemberRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectMemberSchema(result.front()).toJson();
}
