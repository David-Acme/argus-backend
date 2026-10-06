#include "guard-module-data.hxx"

#include <auth/role-access.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <memory>
#include <utility>

namespace
{
bool owned(const std::string& moduleId)
{
  return moduleId == role_access::kSurveillanceModule;
}
}

GuardModuleData::GuardModuleData() : GuardModuleData(GuardModuleDataInput{}) {}

GuardModuleData::GuardModuleData(GuardModuleDataInput input) : input_(std::move(input))
{
  if (!input_.storageReady)
    input_.storageReady = [this] { return storage_.isConfigured(); };
  if (!input_.removeObject)
    input_.removeObject = [this](std::string key) -> drogon::Task<void> { co_await storage_.remove(key); };
}

ModuleDataSummary GuardModuleData::summary(const std::string& moduleId) const
{
  return drogon::sync_wait(summaryAsync(moduleId));
}

ModuleDataPurge GuardModuleData::purge(const std::string& moduleId)
{
  return drogon::sync_wait(purgeAsync(moduleId));
}

drogon::Task<ModuleDataSummary> GuardModuleData::summaryAsync(std::string moduleId) const
{
  if (!owned(moduleId))
    co_return ModuleDataSummary{};
  const auto client = DbService::client();
  co_return co_await repository_.summary(client.get());
}

drogon::Task<ModuleDataPurge> GuardModuleData::purgeAsync(std::string moduleId) const
{
  if (!owned(moduleId))
    co_return ModuleDataPurge{.purged = true, .reason = {}};
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await db_transaction::begin(DbService::client());
    co_await repository_.purge(transaction.get());
    if (!co_await db_transaction::Commit(std::move(transaction)))
      co_return ModuleDataPurge{.purged = false, .reason = "not_committed"};
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  LOG_INFO << "Modules: the surveillance data of argus-guard was purged";
  co_return co_await removeEvidenceObjects();
}

drogon::Task<ModuleDataPurge> GuardModuleData::removeEvidenceObjects() const
{
  const auto client = DbService::client();
  if (co_await repository_.countPendingEvidence(client.get()) == 0)
    co_return ModuleDataPurge{.purged = true, .reason = {}};
  if (!input_.storageReady())
    co_return ModuleDataPurge{.purged = false, .reason = "storage_unavailable"};
  const auto deadline = std::chrono::steady_clock::now() + input_.objectBudget;
  bool failed = false;
  while (!failed && std::chrono::steady_clock::now() < deadline) {
    const auto pending = co_await repository_.pendingEvidence(client.get());
    if (pending.empty())
      break;
    for (const auto& evidence : pending) {
      try {
        co_await input_.removeObject(evidence.objectKey);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Modules: an evidence object could not be removed: " << error.what();
        failed = true;
        break;
      }
      co_await repository_.forgetEvidence(evidence.id, client.get());
      if (std::chrono::steady_clock::now() >= deadline)
        break;
    }
  }
  if (co_await repository_.countPendingEvidence(client.get()) == 0)
    co_return ModuleDataPurge{.purged = true, .reason = {}};
  co_return ModuleDataPurge{.purged = false, .reason = failed ? "storage_failed" : "objects_pending"};
}
