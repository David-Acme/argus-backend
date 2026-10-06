#pragma once

#include <drogon/orm/DbClient.h>
#include <feature/modules/repositories/module-state/module-state-query.hxx>
#include <feature/modules/schemas/module-state.hxx>

#include <vector>

class ModuleStateRepository
{
public:
  explicit ModuleStateRepository(drogon::orm::DbClientPtr client);

  [[nodiscard]] std::vector<ModuleStateSchema> findAll() const;
  [[nodiscard]] ModuleStateSchema upsert(const ModuleStateUpsertInput& input) const;

private:
  drogon::orm::DbClientPtr client_;
};
