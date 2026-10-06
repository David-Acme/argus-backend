#pragma once

#include <drogon/orm/DbClient.h>
#include <feature/modules/repositories/module-job/module-job-query.hxx>

#include <cstdint>
#include <optional>
#include <vector>

class ModuleJobRepository
{
public:
  explicit ModuleJobRepository(drogon::orm::DbClientPtr client);

  [[nodiscard]] ModuleJobSchema create(const ModuleJobCreateInput& input) const;
  [[nodiscard]] std::optional<ModuleJobSchema> update(std::int64_t id, const ModuleJobUpdateInput& input) const;
  [[nodiscard]] std::optional<ModuleJobSchema> findById(std::int64_t id) const;
  [[nodiscard]] std::vector<ModuleJobSchema> findOpen() const;
  [[nodiscard]] std::vector<ModuleJobSchema> findLatestPerModule() const;

private:
  [[nodiscard]] std::vector<ModuleJobSchema> select(const std::string& sql) const;

  drogon::orm::DbClientPtr client_;
};
