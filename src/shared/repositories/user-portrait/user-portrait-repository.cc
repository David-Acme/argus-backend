#include "user-portrait-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace user_portrait_query;

drogon::Task<std::optional<UserPortraitSchema>>
UserPortraitRepository::findByUserId(int64_t userId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_USER_ID.data(), userId);
  if (result.empty())
    co_return std::nullopt;
  co_return UserPortraitSchema(result.front());
}

drogon::Task<UserPortraitSchema>
UserPortraitRepository::upsertCurrent(const UserPortraitUpsertInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(UPSERT_CURRENT.data(), input.userId, input.fileId);

  auto portrait = co_await findByUserId(input.userId);
  if (portrait)
    co_return *portrait;

  UserPortraitSchema schema;
  schema.userId = input.userId;
  schema.fileId = input.fileId;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}
