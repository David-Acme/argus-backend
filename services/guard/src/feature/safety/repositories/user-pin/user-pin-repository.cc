#include "user-pin-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace user_pin_query;

drogon::Task<std::optional<UserPinRow>> UserPinRepository::find(int64_t userId) const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(SELECT_PIN), userId);
  if (rows.empty())
    co_return std::nullopt;
  const auto& row = rows.front();
  co_return UserPinRow{.userId = row["user_id"].as<int64_t>(),
                       .disarmHash = row["disarm_hash"].as<std::string>(),
                       .duressHash = row["duress_hash"].as<std::string>(),
                       .updatedAt = row["updated_at"].as<int64_t>()};
}

drogon::Task<void> UserPinRepository::upsert(const UserPinUpsertInput& input) const
{
  co_await DbService::client()->execSqlCoro(std::string(UPSERT_PIN), input.userId,
                                            input.disarmHash, input.duressHash,
                                            input.now, input.now);
}

drogon::Task<void> UserPinRepository::remove(int64_t userId) const
{
  co_await DbService::client()->execSqlCoro(std::string(DELETE_PIN), userId);
}

drogon::Task<void> UserPinRepository::removeAll() const
{
  co_await DbService::client()->execSqlCoro(std::string(DELETE_ALL_PINS));
}
