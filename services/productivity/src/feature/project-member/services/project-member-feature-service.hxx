#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/project-member/dtos/create-project-member-dto.hxx>
#include <feature/project-member/dtos/update-project-member-dto.hxx>
#include <optional>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/schemas/project-member/project-member-schema.hxx>
#include <sync/user-change-sink.hxx>
#include <auth/user-directory-identity.hxx>
#include <productivity/membership-error.hxx>

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
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<void> emitMembership(const EmitMembershipInput& input) const;

  ProjectMemberRepository repository_;
  ProjectRepository parentRepository_;
  IdentityUserDirectory directory_;
};
