#pragma once

#include "project-member-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <sync/syncable.hxx>
#include <shared/schemas/project-member/project-member-schema.hxx>
#include <vector>

class ProjectMemberRepository : public Syncable
{
public:
  ProjectMemberRepository() = default;
  ~ProjectMemberRepository() override = default;

  drogon::Task<std::optional<ProjectMemberSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<ProjectMemberSchema>> findByParent(int64_t parentId) const;
  drogon::Task<std::optional<ShareAccess>>
  findAccess(const ProjectMemberLookupInput& input) const;
  drogon::Task<std::vector<int64_t>>
  memberIds(int64_t parentId, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::optional<ProjectMemberSchema>>
  findExisting(const ProjectMemberLookupInput& input) const;
  drogon::Task<ProjectMemberSchema> create(const ProjectMemberCreateInput& input) const;
  drogon::Task<ProjectMemberSchema>
  updateAccess(const ProjectMemberUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id,
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
