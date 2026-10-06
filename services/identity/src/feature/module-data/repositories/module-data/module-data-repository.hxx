#pragma once

#include "module-data-query.hxx"

#include <drogon/utils/coroutine.h>
#include <settings/component-vocabulary.hxx>

class ModuleDataRepository
{
public:
  [[nodiscard]] drogon::Task<ModuleDataSummary> summary(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<PurgedVisitors> purgeVisitors(drogon::orm::DbClient* client) const;
};
