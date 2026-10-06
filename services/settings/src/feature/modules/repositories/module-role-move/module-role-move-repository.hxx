#pragma once

#include <drogon/orm/DbClient.h>
#include <feature/modules/repositories/module-role-move/module-role-move-query.hxx>

#include <vector>

class ModuleRoleMoveRepository
{
public:
  explicit ModuleRoleMoveRepository(drogon::orm::DbClientPtr client);

  void create(const ModuleRoleMoveCreateInput& input) const;
  [[nodiscard]] std::vector<ModuleRoleMoveRow> findAll() const;

private:
  drogon::orm::DbClientPtr client_;
};
