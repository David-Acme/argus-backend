#include "identity-module-data.hxx"

#include <auth/role-access.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/object-deletion/object-deletion-worker.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

#include <memory>
#include <utility>

namespace
{
bool owned(const std::string& moduleId)
{
  return moduleId == role_access::kSurveillanceModule;
}
}

IdentityModuleData::IdentityModuleData() : IdentityModuleData(IdentityModuleDataInput{}) {}

IdentityModuleData::IdentityModuleData(IdentityModuleDataInput input) : input_(std::move(input))
{
  if (!input_.forgetVectors)
    input_.forgetVectors = [](const std::vector<std::int64_t>& ids) {
      FaceService::instance().faceDb().removeEmbeddings(ids);
    };
  if (!input_.kickDeletion)
    input_.kickDeletion = [] { ObjectDeletionWorker::instance().kick(); };
}

ModuleDataSummary IdentityModuleData::summary(const std::string& moduleId) const
{
  return drogon::sync_wait(summaryAsync(moduleId));
}

ModuleDataPurge IdentityModuleData::purge(const std::string& moduleId)
{
  return drogon::sync_wait(purgeAsync(moduleId));
}

drogon::Task<ModuleDataSummary> IdentityModuleData::summaryAsync(std::string moduleId) const
{
  if (!owned(moduleId))
    co_return ModuleDataSummary{};
  const auto client = DbService::identityClient();
  co_return co_await repository_.summary(client.get());
}

drogon::Task<ModuleDataPurge> IdentityModuleData::purgeAsync(std::string moduleId) const
{
  if (!owned(moduleId))
    co_return ModuleDataPurge{.purged = true, .reason = {}};
  std::shared_ptr<drogon::orm::Transaction> transaction;
  PurgedVisitors purged;
  try {
    transaction = co_await db_transaction::begin(DbService::identityClient());
    purged = co_await repository_.purgeVisitors(transaction.get());
    if (!purged.cropKeys.empty())
      co_await pendingRepository_.enqueue({.objectKeys = purged.cropKeys, .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      co_return ModuleDataPurge{.purged = false, .reason = "not_committed"};
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (!purged.embeddingIds.empty())
    co_await BlockingTask<void>(
        [forget = input_.forgetVectors, ids = std::move(purged.embeddingIds)] { forget(ids); });
  if (!purged.cropKeys.empty())
    input_.kickDeletion();
  LOG_INFO << "Modules: the visitors of argus-identity were purged (" << purged.visitors << " visitors, "
           << purged.cropKeys.size() << " crops queued for deletion)";
  co_return ModuleDataPurge{.purged = true, .reason = {}};
}
