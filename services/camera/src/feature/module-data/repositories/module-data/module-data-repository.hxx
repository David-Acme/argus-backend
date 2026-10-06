#pragma once

#include "module-data-query.hxx"

#include <drogon/utils/coroutine.h>
#include <settings/component-vocabulary.hxx>

#include <vector>

class ModuleDataRepository
{
public:
  [[nodiscard]] drogon::Task<ModuleDataSummary> summary(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<std::vector<std::int64_t>> purge(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<std::vector<PendingEvidence>> pendingEvidence(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<std::int64_t> countPendingEvidence(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<void> forgetEvidence(std::int64_t id, drogon::orm::DbClient* client) const;
};
