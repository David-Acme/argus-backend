#pragma once

#include "project-query.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/project/project-schema.hxx>
#include <vector>

class ProjectRepository : public Syncable
{
public:
  ProjectRepository() = default;
  ~ProjectRepository() override = default;

  drogon::Task<std::optional<ProjectSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<ProjectSchema>> findByOwner(int64_t ownerId) const;
  drogon::Task<ProjectSchema> create(const ProjectCreateInput& input) const;
  drogon::Task<ProjectSchema> update(int64_t id,
                                     const ProjectUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
