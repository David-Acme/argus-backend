#include "sync-module-data.hxx"

#include <shared/vocabulary/module-tables.hxx>
#include <json/value.h>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <array>
#include <memory>
#include <utility>

namespace
{
std::string tableList(std::span<const TableName> tables)
{
  Json::Value list(Json::arrayValue);
  for (const auto table : tables)
    list.append(tableNameToString(table));
  return json_util::toString(list);
}
}

std::span<const TableName> SyncModuleData::tablesOf(std::string_view moduleId)
{
  return module_tables::tablesOf(moduleId);
}

ModuleDataSummary SyncModuleData::summary(const std::string& moduleId) const
{
  return drogon::sync_wait(summaryAsync(moduleId));
}

ModuleDataPurge SyncModuleData::purge(const std::string& moduleId)
{
  return drogon::sync_wait(purgeAsync(moduleId));
}

drogon::Task<ModuleDataSummary> SyncModuleData::summaryAsync(std::string moduleId) const
{
  const auto tables = tablesOf(moduleId);
  if (tables.empty())
    co_return ModuleDataSummary{};
  const auto client = DbService::client();
  co_return co_await repository_.summary({.tables = tableList(tables), .client = client.get()});
}

drogon::Task<ModuleDataPurge> SyncModuleData::purgeAsync(std::string moduleId) const
{
  const auto tables = tablesOf(moduleId);
  if (tables.empty())
    co_return ModuleDataPurge{.purged = true, .reason = {}};
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await db_transaction::begin(DbService::client());
    co_await repository_.purge({.tables = tableList(tables), .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      co_return ModuleDataPurge{.purged = false, .reason = "not_committed"};
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  LOG_INFO << "Modules: the change history of " << moduleId << " was purged";
  co_return ModuleDataPurge{.purged = true, .reason = {}};
}
