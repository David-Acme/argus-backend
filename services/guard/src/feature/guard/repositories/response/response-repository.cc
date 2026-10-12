#include "response-repository.hxx"

#include <exception>
#include <memory>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <stdexcept>
#include <string>
#include <trantor/utils/Logger.h>
#include <utility>
#include <vector>

using namespace response_query;

namespace
{
std::string joinedRows(std::string_view head, std::string_view row,
                       std::size_t count)
{
  std::string sql(head);
  sql.reserve(head.size() + count * (row.size() + 2));
  for (std::size_t index = 0; index < count; ++index) {
    if (index > 0)
      sql += ", ";
    sql += row;
  }
  return sql;
}
}

drogon::Task<ResponseConfigRows>
ResponseRepository::forEnvironment(int64_t environmentId) const
{
  auto client = DbService::client();
  ResponseConfigRows config;
  config.setting.environmentId = environmentId;
  for (const auto& row :
       co_await client->execSqlCoro(std::string(LIST_RECIPIENTS),
                                    environmentId)) {
    ResponseRecipientRow recipient{.environmentId =
                                       row["environment_id"].as<int64_t>(),
                                   .userId = row["user_id"].as<int64_t>(),
                                   .mode = std::nullopt,
                                   .step = std::nullopt,
                                   .onDuty = row["on_duty"].as<int64_t>() != 0};
    if (!row["mode"].isNull())
      recipient.mode = recipientModeFromString(row["mode"].as<std::string>());
    if (!row["step"].isNull())
      recipient.step = row["step"].as<int>();
    config.recipients.push_back(recipient);
  }
  for (const auto& row :
       co_await client->execSqlCoro(std::string(LIST_CONTACTS), environmentId))
    config.contacts.push_back(
        {.id = row["id"].as<int64_t>(),
         .environmentId = row["environment_id"].as<int64_t>(),
         .position = row["position"].as<int>(),
         .name = row["name"].as<std::string>(),
         .phone = row["phone"].as<std::string>(),
         .note = row["note"].as<std::string>()});
  const auto setting =
      co_await client->execSqlCoro(std::string(FIND_SETTING), environmentId);
  if (!setting.empty()) {
    config.setting.emergencyNumber =
        setting[0]["emergency_number"].as<std::string>();
    config.setting.stepSeconds = setting[0]["step_seconds"].as<int>();
  }
  co_return config;
}

drogon::Task<void>
ResponseRepository::writeReplace(const ResponseReplaceInput& input,
                                 drogon::orm::DbClient* client) const
{
  const std::string at = std::to_string(input.at);
  const std::string environment = std::to_string(input.environmentId);
  std::vector<std::string> recipientArgs;
  recipientArgs.reserve(input.recipients.size() * 6);
  for (const auto& recipient : input.recipients) {
    recipientArgs.push_back(environment);
    recipientArgs.push_back(std::to_string(recipient.userId));
    recipientArgs.push_back(recipientModeToString(recipient.mode));
    recipientArgs.push_back(std::to_string(recipient.step));
    recipientArgs.emplace_back(recipient.onDuty ? "1" : "0");
    recipientArgs.push_back(at);
  }
  std::vector<std::string> contactArgs;
  contactArgs.reserve(input.contacts.size() * 6);
  for (std::size_t index = 0; index < input.contacts.size(); ++index) {
    const auto& contact = input.contacts[index];
    contactArgs.push_back(environment);
    contactArgs.push_back(std::to_string(index));
    contactArgs.push_back(contact.name);
    contactArgs.push_back(contact.phone);
    contactArgs.push_back(contact.note);
    contactArgs.push_back(at);
  }
  const auto& recipientArgsRef = recipientArgs;
  const auto& contactArgsRef = contactArgs;
  const std::string recipientSql =
      joinedRows(INSERT_RECIPIENT_HEAD, INSERT_RECIPIENT_ROW,
                 input.recipients.size());
  const std::string contactSql =
      joinedRows(INSERT_CONTACT_HEAD, INSERT_CONTACT_ROW,
                 input.contacts.size());

  co_await client->execSqlCoro(std::string(DELETE_RECIPIENTS),
                               input.environmentId);
  co_await client->execSqlCoro(std::string(DELETE_CONTACTS),
                               input.environmentId);
  if (!input.recipients.empty())
    co_await client->execSqlCoro(recipientSql, recipientArgsRef);
  if (!input.contacts.empty())
    co_await client->execSqlCoro(contactSql, contactArgsRef);
  co_await client->execSqlCoro(std::string(UPSERT_SETTING),
                               input.environmentId, input.emergencyNumber,
                               input.stepSeconds, input.at);
}

drogon::Task<void>
ResponseRepository::replace(const ResponseReplaceInput& input) const
{
  if (input.client != nullptr) {
    co_await writeReplace(input, input.client);
    co_return;
  }
  std::shared_ptr<drogon::orm::Transaction> transaction =
      co_await db_transaction::begin(DbService::client());
  try {
    co_await writeReplace(input, transaction.get());
  }
  catch (const std::exception& error) {
    transaction->rollback();
    LOG_WARN << "Guard response replace failed: " << error.what();
    throw;
  }
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw std::runtime_error("response replace did not commit");
}

drogon::Task<void>
ResponseRepository::setDuty(const ResponseDutyInput& input) const
{
  const auto owned = DbService::client();
  drogon::orm::DbClient* client =
      input.client != nullptr ? input.client : owned.get();
  co_await client->execSqlCoro(std::string(UPSERT_DUTY), input.environmentId,
                               input.userId, input.onDuty ? 1 : 0, input.at);
}
