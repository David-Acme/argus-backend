#pragma once

#include <drogon/orm/DbClient.h>
#include <feature/modules/repositories/module-purge/module-purge-query.hxx>

#include <set>
#include <string>

class ModulePurgeRepository
{
public:
  explicit ModulePurgeRepository(drogon::orm::DbClientPtr client);

  [[nodiscard]] std::set<std::string> purgedOwners(const std::string& moduleId) const;
  void create(const ModulePurgeCreateInput& input) const;
  void clear(const std::string& moduleId) const;

private:
  drogon::orm::DbClientPtr client_;
};
