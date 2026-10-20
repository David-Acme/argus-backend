#include "user-minor-repository.hxx"

#include <sqlite/db-service.hxx>
#include <string>

using namespace user_minor_query;

drogon::Task<std::optional<UserMinorSchema>>
UserMinorRepository::find(int64_t userId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(std::string{FIND_USER}, userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return UserMinorSchema(rows.front());
}

drogon::Task<std::vector<UserMinorSchema>> UserMinorRepository::findAll(drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(std::string{FIND_ALL});
  std::vector<UserMinorSchema> records;
  records.reserve(rows.size());
  for (const auto& row : rows)
    records.emplace_back(row);
  co_return records;
}

drogon::Task<std::optional<UserMinorSchema>> UserMinorRepository::upsert(const UserMinorUpsertInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const int64_t consent = input.guardianConsent ? 1 : 0;
  co_await client->execSqlCoro(std::string{UPSERT}, input.userId, consent, input.guardianId, input.setBy);
  co_return co_await find(input.userId, client);
}

drogon::Task<bool> UserMinorRepository::remove(int64_t userId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto result = co_await resolved->execSqlCoro(std::string{REMOVE}, userId);
  co_return result.affectedRows() > 0;
}
