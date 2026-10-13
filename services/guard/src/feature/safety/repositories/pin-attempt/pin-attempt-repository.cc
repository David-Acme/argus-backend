#include "pin-attempt-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace pin_attempt_query;

drogon::Task<int64_t> PinAttemptRepository::failuresSince(PinAttemptWindowInput input) const
{
  const auto client = DbService::client();
  co_await client->execSqlCoro(std::string(PRUNE_USER), input.userId, input.since);
  const auto rows = co_await client->execSqlCoro(std::string(COUNT_SINCE), input.userId, input.since);
  co_return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

drogon::Task<void> PinAttemptRepository::recordFailure(PinFailureInput input) const
{
  co_await DbService::client()->execSqlCoro(std::string(INSERT_FAILURE), input.userId, input.at);
}

drogon::Task<void> PinAttemptRepository::clear(int64_t userId) const
{
  co_await DbService::client()->execSqlCoro(std::string(CLEAR_USER), userId);
}

drogon::Task<int64_t> PinAttemptRepository::purgeBefore(int64_t cutoff) const
{
  const auto result = co_await DbService::client()->execSqlCoro(std::string(PURGE_BEFORE), cutoff);
  co_return static_cast<int64_t>(result.affectedRows());
}
