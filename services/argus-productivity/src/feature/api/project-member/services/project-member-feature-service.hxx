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
  struct UpdateInput
  {
    int64_t id{0};
    const UpdateProjectMemberDto& body;
    int64_t actorId{0};
  };

  /** Only the owner of the parent record may share it. */
  drogon::Task<ProjectMemberResult> create(const CreateProjectMemberDto& body,
                                    int64_t actorId) const;
  drogon::Task<ProjectMemberResult> update(const UpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  struct EmitMembershipInput
  {
    SyncOperation operation{};
    const ProjectMemberSchema& row;
    int64_t ownerId{0};
  };

  struct EmitParentInput
  {
    SyncOperation operation{};
    int64_t parentId{0};
    int64_t userId{0};
  };

  // Emits the membership row to both sides and the parent record to the member.
  void emitMembership(const EmitMembershipInput& input) const;
  drogon::Task<void> emitParent(const EmitParentInput& input) const;

  ProjectMemberRepository repository_;
  ProjectRepository parentRepository_;
  UserRepository userRepository_;
};
