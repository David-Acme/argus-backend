#pragma once

#include "project-member-query.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/project-member/project-member-schema.hxx>
#include <vector>

class ProjectMemberRepository : public Syncable
{
public:
  ProjectMemberRepository() = default;
  ~ProjectMemberRepository() override = default;

  drogon::Task<std::optional<ProjectMemberSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<ProjectMemberSchema>> findByParent(int64_t parentId) const;
  /** Access level of one member, or nullopt when the record is not shared with them. */
  drogon::Task<std::optional<ShareAccess>> findAccess(int64_t parentId,
                                                      int64_t userId) const;
  /** Ids the record is shared with, so a write can be emitted to all of them. */
  drogon::Task<std::vector<int64_t>> memberIds(int64_t parentId) const;
  drogon::Task<std::optional<ProjectMemberSchema>> findExisting(int64_t parentId,
                                                         int64_t userId) const;
  drogon::Task<ProjectMemberSchema> create(const ProjectMemberCreateInput& input) const;
  drogon::Task<ProjectMemberSchema> updateAccess(int64_t id, ShareAccess access) const;
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
