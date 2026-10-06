#pragma once

#include "module-data-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <settings/component-vocabulary.hxx>

#include <cstdint>

class ModuleDataRepository
{
public:
  [[nodiscard]] drogon::Task<ModuleDataSummary> summary(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<void> purge(drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<std::int64_t> upcomingEvents(drogon::orm::DbClient* client, std::int64_t now) const;
};
