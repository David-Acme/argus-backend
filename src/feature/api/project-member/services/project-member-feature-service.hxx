#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/project-member/dtos/create-project-member-dto.hxx>
#include <feature/api/project-member/dtos/update-project-member-dto.hxx>
#include <optional>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/schemas/project-member/project-member-schema.hxx>
#include <shared/contracts/user-change-sink.hxx>

struct ProjectMemberResult
{
  MembershipError error{MembershipError::None};
  std::optional<ProjectMemberSchema> row;
};

class ProjectMemberFeatureService
{
public:
  /** Only the owner of the parent record may share it. */
  drogon::Task<ProjectMemberResult> create(const CreateProjectMemberDto& body,
                                    int64_t actorId) const;
  drogon::Task<ProjectMemberResult> update(int64_t id, const UpdateProjectMemberDto& body,
                                    int64_t actorId) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  // The membership row goes to both sides of the share. The parent record goes
  // to the member as well: without it the share would only show up on their
  // device after a full resync.
  void emitMembership(SyncOperation operation, const ProjectMemberSchema& row,
                      int64_t ownerId) const;
  drogon::Task<void> emitParent(SyncOperation operation, int64_t parentId,
                                int64_t userId) const;

  ProjectMemberRepository repository_;
  ProjectRepository parentRepository_;
  UserRepository userRepository_;
};
