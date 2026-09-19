#include "user-invitation-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace user_invitation_query;

drogon::Task<UserInvitationSchema>
UserInvitationRepository::create(const UserInvitationCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.tokenHash, userRoleToString(input.role),
      input.maxRedemptions, input.expiresAt, input.createdBy);

  UserInvitationSchema schema;
  schema.id = result.insertId();
  schema.tokenHash = input.tokenHash;
  schema.role = input.role;
  schema.maxRedemptions = input.maxRedemptions;
  schema.expiresAt = input.expiresAt;
  schema.createdBy = input.createdBy;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<UserInvitationSchema>>
UserInvitationRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return UserInvitationSchema(result.front());
}

drogon::Task<std::optional<UserInvitationSchema>>
UserInvitationRepository::findByTokenHash(const std::string& tokenHash) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_TOKEN_HASH.data(), tokenHash);
  if (result.empty())
    co_return std::nullopt;
  co_return UserInvitationSchema(result.front());
}

drogon::Task<std::vector<UserInvitationSchema>>
UserInvitationRepository::findAll() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_ALL.data());
  std::vector<UserInvitationSchema> invitations;
  invitations.reserve(result.size());
  for (const auto& row : result)
    invitations.emplace_back(row);
  co_return invitations;
}

drogon::Task<bool>
UserInvitationRepository::revoke(const UserInvitationRevokeInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      REVOKE.data(), input.revokedBy, input.invitationId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
UserInvitationRepository::tryConsume(int64_t invitationId, int64_t now) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(TRY_CONSUME.data(), invitationId, now);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
UserInvitationRepository::recordRedemption(
    const InvitationRedemptionCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT_REDEMPTION.data(), input.invitationId, input.userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
UserInvitationRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto [query, args] = sync_query::buildSyncQuery({.filter = filter,
                                                         .queryBoth = FIND_SYNC,
                                                         .queryFrom = FIND_SYNC_FROM,
                                                         .queryAll = FIND_SYNC_ALL,
                                                         .queryAfterBoth = FIND_SYNC_AFTER,
                                                         .queryAfterFrom = FIND_SYNC_AFTER_FROM});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);
  std::vector<Json::Value> data;
  data.reserve(rows.size());
  for (const auto& row : rows)
    data.push_back(UserInvitationSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
UserInvitationRepository::findDeleted(const SyncFilter&) const
{
  co_return std::vector<Json::Value>{};
}

drogon::Task<std::optional<Json::Value>>
UserInvitationRepository::findLast(const SyncFilter&) const
{
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(FIND_SYNC_LAST.data());
  if (rows.empty())
    co_return std::nullopt;
  co_return UserInvitationSchema(rows.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
UserInvitationRepository::findLastDeleted(const SyncFilter&) const
{
  co_return std::nullopt;
}
