#include "notification-token-repository.hxx"

#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>

#include <stdexcept>
#include <string>

using namespace notification_token_query;

drogon::Task<void>
NotificationTokenRepository::upsert(const NotificationTokenCreateInput& input) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    co_await transaction->execSqlCoro(RELEASE_TOKEN.data(), input.token,
                                      input.userId, input.deviceHash);
    co_await transaction->execSqlCoro(UPSERT.data(), input.userId,
                                      input.deviceHash, input.token,
                                      input.platform, input.lang,
                                      input.sessionId);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw std::runtime_error("notification token upsert did not commit");
}

drogon::Task<std::vector<NotificationTokenSchema>>
NotificationTokenRepository::findByUser(int64_t userId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_USER.data(), userId);
  std::vector<NotificationTokenSchema> tokens;
  tokens.reserve(result.size());
  for (const auto& row : result)
    tokens.emplace_back(row);
  co_return tokens;
}

drogon::Task<int64_t> NotificationTokenRepository::removeForSession(
    const NotificationTokenSessionInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(DELETE_FOR_SESSION), input.userId, input.sessionId);
  co_return static_cast<int64_t>(result.affectedRows());
}

drogon::Task<int64_t>
NotificationTokenRepository::removeForUser(int64_t userId) const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(DELETE_FOR_USER), userId);
  co_return static_cast<int64_t>(result.affectedRows());
}
