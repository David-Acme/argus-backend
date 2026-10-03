#pragma once

#include "project-task-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <sync/syncable.hxx>
#include <shared/schemas/project-task/project-task-schema.hxx>
#include <vector>

class ProjectTaskRepository : public Syncable
{
public:
  ProjectTaskRepository() = default;
  ~ProjectTaskRepository() override = default;

  drogon::Task<std::optional<ProjectTaskSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<ProjectTaskSchema>>
  findByProject(int64_t projectId,
                drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<ProjectTaskSchema>
  create(const ProjectTaskCreateInput& input) const;
  drogon::Task<ProjectTaskSchema>
  update(int64_t id, const ProjectTaskUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id,
                            drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<void> removeByProject(int64_t projectId,
                                     drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
