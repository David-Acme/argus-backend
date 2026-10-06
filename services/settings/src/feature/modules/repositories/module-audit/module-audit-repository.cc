#include "module-audit-repository.hxx"

#include <utility>

using namespace module_audit_query;

ModuleAuditRepository::ModuleAuditRepository(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

ModuleAuditSchema ModuleAuditRepository::create(const ModuleAuditCreateInput& input) const
{
  const auto result = client_->execSqlSync(INSERT, input.moduleId, std::string(moduleAuditActionToString(input.action)),
                                           input.userId, input.detail, input.at);
  return {.id = static_cast<std::int64_t>(result.insertId()),
          .moduleId = input.moduleId,
          .action = input.action,
          .userId = input.userId,
          .detail = input.detail,
          .createdAt = input.at};
}

std::int64_t ModuleAuditRepository::enabledVersion() const
{
  const auto rows = client_->execSqlSync(SELECT_ENABLED_VERSION);
  return rows.empty() ? 0 : rows.front()["version"].as<std::int64_t>();
}

namespace
{
std::vector<ModuleAuditSchema> entriesOf(const drogon::orm::Result& rows)
{
  std::vector<ModuleAuditSchema> entries;
  entries.reserve(rows.size());
  for (const auto& row : rows)
    entries.push_back({.id = row["id"].as<std::int64_t>(),
                       .moduleId = row["module_id"].as<std::string>(),
                       .action = moduleAuditActionFromString(row["action"].as<std::string>())
                                     .value_or(ModuleAuditAction::InstallRequested),
                       .userId = row["user_id"].as<std::int64_t>(),
                       .detail = row["detail"].as<std::string>(),
                       .createdAt = row["created_at"].as<std::int64_t>()});
  return entries;
}
}

std::int64_t ModuleAuditRepository::lastId() const
{
  const auto rows = client_->execSqlSync(SELECT_LAST_ID);
  return rows.empty() ? 0 : rows.front()["last"].as<std::int64_t>();
}

std::vector<ModuleAuditSchema> ModuleAuditRepository::findAfter(const ModuleAuditAfterInput& input) const
{
  return entriesOf(client_->execSqlSync(SELECT_AFTER, input.afterId, static_cast<std::int64_t>(input.limit)));
}

std::vector<ModuleAuditSchema> ModuleAuditRepository::findByModule(const std::string& moduleId) const
{
  return entriesOf(client_->execSqlSync(SELECT_BY_MODULE, moduleId));
}
