#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/project/dtos/create-project-dto.hxx>
#include <feature/api/project/dtos/update-project-dto.hxx>
#include <optional>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/schemas/project/project-schema.hxx>
#include <shared/contracts/user-change-sink.hxx>

class ProjectFeatureService
{
public:
  struct UpdateInput
  {
    int64_t id{0};
    const UpdateProjectDto& body;
    int64_t actorId{0};
  };

  drogon::Task<ProjectSchema> create(const CreateProjectDto& body,
                                     int64_t ownerId) const;
  drogon::Task<std::optional<ProjectSchema>>
  update(const UpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  // Pushes every mutation to the owner's room so the client updates without polling.
  drogon::Task<void> emit(SyncOperation operation,
                          const ProjectSchema& row) const;
  /** True for the owner and for a member whose membership says `edit`. */
  drogon::Task<bool> canEdit(const ProjectSchema& row, int64_t actorId) const;

  ProjectRepository repository_;
  ProjectMemberRepository memberRepository_;
};
