#pragma once

#include <drogon/orm/DbClient.h>
#include <feature/modules/repositories/module-audit/module-audit-query.hxx>

#include <cstdint>
#include <string>
#include <vector>

class ModuleAuditRepository
{
public:
  explicit ModuleAuditRepository(drogon::orm::DbClientPtr client);

  [[nodiscard]] ModuleAuditSchema create(const ModuleAuditCreateInput& input) const;
  [[nodiscard]] std::int64_t enabledVersion() const;
  [[nodiscard]] std::int64_t lastId() const;
  [[nodiscard]] std::vector<ModuleAuditSchema> findAfter(const ModuleAuditAfterInput& input) const;
  [[nodiscard]] std::vector<ModuleAuditSchema> findByModule(const std::string& moduleId) const;

private:
  drogon::orm::DbClientPtr client_;
};
