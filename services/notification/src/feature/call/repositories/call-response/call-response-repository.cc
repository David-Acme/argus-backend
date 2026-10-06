#include "call-response-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace call_response_query;

namespace
{
std::vector<CallResponseSchema> responsesOf(const drogon::orm::Result& result)
{
  std::vector<CallResponseSchema> responses;
  responses.reserve(result.size());
  for (const auto& row : result)
    responses.push_back(CallResponseSchema::fromRow(row));
  return responses;
}

std::string membersSql(std::size_t count)
{
  std::string sql(INSERT_MEMBERS_HEAD);
  for (std::size_t index = 0; index < count; ++index) {
    if (index > 0)
      sql += ", ";
    sql += INSERT_MEMBER_ROW;
  }
  return sql;
}
}

drogon::Task<CallResponseOpened>
CallResponseRepository::open(const CallResponseCreateInput& input,
                             const std::vector<CallResponseMemberInput>& members) const
{
  CallResponseOpened opened;
  std::vector<std::string> args;
  args.reserve(members.size() * 6);
  std::shared_ptr<drogon::orm::Transaction> transaction =
      co_await db_transaction::begin(DbService::client());
  try {
    const auto rows = co_await transaction->execSqlCoro(
        std::string(INSERT), input.dedupeKey, input.kind, input.environmentId,
        input.episodeId, input.cameraId, input.strategy, input.stepCount,
        input.stepSeconds, input.stepDeadline, input.plan, input.data, input.at,
        input.at);
    if (!rows.empty() && !members.empty()) {
      const std::string id = std::to_string(rows.front()["id"].as<int64_t>());
      for (const auto& member : members) {
        args.push_back(id);
        args.push_back(std::to_string(member.userId));
        args.push_back(std::to_string(member.step));
        args.push_back(responseMemberModeToString(member.mode));
        args.emplace_back(member.mandatory ? "1" : "0");
        args.emplace_back(member.discreet ? "1" : "0");
      }
      const auto& argsRef = args;
      co_await transaction->execSqlCoro(membersSql(members.size()), argsRef);
    }
    opened.created = !rows.empty();
  }
  catch (const std::exception& error) {
    transaction->rollback();
    LOG_WARN << "Call response open failed: " << error.what();
    throw;
  }
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw std::runtime_error("call response open did not commit");
  opened.response = co_await findByKey(input.dedupeKey);
  co_return opened;
}

drogon::Task<std::optional<CallResponseSchema>>
CallResponseRepository::findById(int64_t id) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(FIND_BY_ID), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return CallResponseSchema::fromRow(rows.front());
}

drogon::Task<std::optional<CallResponseSchema>>
CallResponseRepository::findByKey(const std::string& dedupeKey) const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(FIND_BY_KEY), dedupeKey);
  if (rows.empty())
    co_return std::nullopt;
  co_return CallResponseSchema::fromRow(rows.front());
}

drogon::Task<std::vector<CallResponseMember>>
CallResponseRepository::members(int64_t responseId) const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(MEMBERS), responseId);
  std::vector<CallResponseMember> members;
  members.reserve(rows.size());
  for (const auto& row : rows)
    members.push_back(CallResponseMember::fromRow(row));
  co_return members;
}

drogon::Task<std::optional<CallResponseMember>>
CallResponseRepository::member(int64_t responseId, int64_t userId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(MEMBER),
                                                               responseId, userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return CallResponseMember::fromRow(rows.front());
}

drogon::Task<bool> CallResponseRepository::markReached(const CallResponseReachInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(MARK_REACHED), input.at, input.responseId, input.userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<CallResponseSchema>>
CallResponseRepository::due(const CallResponseDueInput& input) const
{
  co_return responsesOf(co_await DbService::client()->execSqlCoro(
      std::string(DUE), input.now, input.limit));
}

drogon::Task<bool> CallResponseRepository::advance(const CallResponseAdvanceInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(ADVANCE), input.toStep, input.deadline, input.at, input.id,
      input.fromStep);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
CallResponseRepository::setDeadline(const CallResponseAdvanceInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(SET_DEADLINE), input.deadline, input.at, input.id, input.fromStep);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallResponseRepository::markUnanswered(int64_t id, int64_t at) const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(MARK_UNANSWERED), at, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallResponseRepository::attend(const CallResponseAttendInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(ATTEND), input.userId, input.name, input.at, input.id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallResponseRepository::verdict(const CallResponseVerdictInput& input) const
{
  const bool real = input.verdict == ResponseVerdict::Real;
  const std::string sql =
      std::string(VERDICT_HEAD) +
      std::string(real ? VERDICT_FROM_OPEN : VERDICT_FROM_CONFIRMED);
  const auto result = co_await DbService::client()->execSqlCoro(
      sql,
      responseStateToString(real ? ResponseState::Confirmed : ResponseState::FalseAlarm),
      responseVerdictToString(input.verdict), input.userId, input.name, input.at,
      input.name, input.userId, input.at, input.id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<int64_t>>
CallResponseRepository::expire(int64_t createdBefore, int64_t at) const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(EXPIRE), at, createdBefore);
  std::vector<int64_t> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows)
    ids.push_back(row["id"].as<int64_t>());
  co_return ids;
}

drogon::Task<std::vector<int64_t>>
CallResponseRepository::expireKinds(const CallResponseExpireKindsInput& input) const
{
  if (input.kinds.empty())
    co_return std::vector<int64_t>{};
  std::string sql(EXPIRE_KINDS_HEAD);
  std::vector<std::string> args{std::to_string(input.at)};
  for (const auto& kind : input.kinds) {
    if (args.size() > 1)
      sql += ", ";
    sql += '?';
    args.push_back(kind);
  }
  sql += EXPIRE_KINDS_TAIL;
  const auto& argsRef = args;
  const auto rows = co_await DbService::client()->execSqlCoro(sql, argsRef);
  std::vector<int64_t> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows)
    ids.push_back(row["id"].as<int64_t>());
  co_return ids;
}

drogon::Task<std::vector<CallResponseForUser>>
CallResponseRepository::forUser(const CallResponseUserInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(FOR_USER), input.userId, input.closedSince, input.limit);
  std::vector<CallResponseForUser> views;
  views.reserve(rows.size());
  for (const auto& row : rows) {
    views.push_back(
        {.response = CallResponseSchema::fromRow(row),
         .member = {.responseId = row["id"].as<int64_t>(),
                    .userId = row["member_user_id"].as<int64_t>(),
                    .step = row["member_step"].as<int>(),
                    .mode = responseMemberModeFromString(
                        row["member_mode"].as<std::string>()),
                    .mandatory = row["member_mandatory"].as<int64_t>() != 0,
                    .discreet = row["member_discreet"].as<int64_t>() != 0,
                    .reachedAt = row["member_reached_at"].as<int64_t>()}});
  }
  co_return views;
}

drogon::Task<int64_t>
CallResponseRepository::purgeClosed(int64_t updatedBefore) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  int64_t purged = 0;
  try {
    co_await transaction->execSqlCoro(std::string(PURGE_CLOSED_MEMBERS),
                                      updatedBefore);
    const auto result = co_await transaction->execSqlCoro(
        std::string(PURGE_CLOSED), updatedBefore);
    purged = static_cast<int64_t>(result.affectedRows());
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw std::runtime_error("call response purge did not commit");
  co_return purged;
}
