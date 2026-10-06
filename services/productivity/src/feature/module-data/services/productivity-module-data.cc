#include "productivity-module-data.hxx"

#include <auth/role-access.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

#include <memory>
#include <utility>

namespace
{
bool owned(const std::string& moduleId)
{
  return moduleId == role_access::kProductivityModule;
}
}

ModuleDataSummary ProductivityModuleData::summary(const std::string& moduleId) const
{
  return drogon::sync_wait(summaryAsync(moduleId));
}

ModuleDataPurge ProductivityModuleData::purge(const std::string& moduleId)
{
  return drogon::sync_wait(purgeAsync(moduleId));
}

drogon::Task<ModuleDataSummary> ProductivityModuleData::summaryAsync(std::string moduleId) const
{
  if (!owned(moduleId))
    co_return ModuleDataSummary{};
  const auto client = DbService::productivityClient();
  co_return co_await repository_.summary(client.get());
}

drogon::Task<ModuleDataPurge> ProductivityModuleData::purgeAsync(std::string moduleId) const
{
  if (!owned(moduleId))
    co_return ModuleDataPurge{.purged = true, .reason = {}};
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await db_transaction::begin(DbService::productivityClient());
    co_await repository_.purge(transaction.get());
    if (!co_await db_transaction::Commit(std::move(transaction)))
      co_return ModuleDataPurge{.purged = false, .reason = "not_committed"};
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  LOG_INFO << "Modules: the productivity data was purged";
  co_return ModuleDataPurge{.purged = true, .reason = {}};
}
