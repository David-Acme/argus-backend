#include "module-journal-repository.hxx"

#include <utility>

using namespace module_journal_query;

ModuleJournalRepository::ModuleJournalRepository(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

std::optional<std::int64_t> ModuleJournalRepository::cursor() const
{
  const auto rows = client_->execSqlSync(SELECT_CURSOR);
  if (rows.empty())
    return std::nullopt;
  return rows.front()["published_through"].as<std::int64_t>();
}

void ModuleJournalRepository::start(std::int64_t publishedThrough) const
{
  client_->execSqlSync(INSERT_CURSOR, publishedThrough);
}

void ModuleJournalRepository::advance(const ModuleJournalAdvanceInput& input) const
{
  client_->execSqlSync(ADVANCE_CURSOR, input.auditId, input.auditId);
}
