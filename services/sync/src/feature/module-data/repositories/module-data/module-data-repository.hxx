#pragma once

#include "module-data-query.hxx"

#include <drogon/utils/coroutine.h>
#include <settings/component-vocabulary.hxx>

class ModuleDataRepository
{
public:
  [[nodiscard]] drogon::Task<ModuleDataSummary> summary(const ModuleHistoryInput& input) const;
  [[nodiscard]] drogon::Task<void> purge(const ModuleHistoryInput& input) const;
};
