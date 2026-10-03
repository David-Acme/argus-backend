#include "idempotency-key-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>

using namespace idempotency_key_query;

namespace
{
int64_t nowSeconds()
{
  return static_cast<int64_t>(std::time(nullptr));
}
}

drogon::Task<std::optional<IdempotencyKeyRecord>>
IdempotencyKeyRepository::find(const IdempotencyKeyFindInput& input) const
{
  const auto pooled = DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND), input.userId, input.key, nowSeconds() - kWindowSeconds);
  if (result.empty())
    co_return std::nullopt;
  co_return IdempotencyKeyRecord{
      .route = result.front()["route"].as<std::string>(),
      .recordId = result.front()["record_id"].as<int64_t>()};
}

drogon::Task<void>
IdempotencyKeyRepository::remember(const IdempotencyKeyRememberInput& input) const
{
  const auto pooled = DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  const int64_t now = nowSeconds();
  co_await client->execSqlCoro(std::string(PURGE), now - kWindowSeconds);
  co_await client->execSqlCoro(std::string(INSERT), input.userId, input.key,
                               input.route, input.recordId, now);
}
