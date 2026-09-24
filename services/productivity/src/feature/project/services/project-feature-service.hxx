#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/project/dtos/create-project-dto.hxx>
#include <feature/project/dtos/update-project-dto.hxx>
#include <optional>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/schemas/project/project-schema.hxx>
#include <sync/user-change-sink.hxx>

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
  struct EmitInput
  {
    SyncOperation operation{};
    const ProjectSchema& row;
    drogon::orm::DbClient* client{nullptr};
  };

  struct CanEditInput
  {
    const ProjectSchema& row;
    int64_t actorId{0};
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<void> emit(const EmitInput& input) const;
  drogon::Task<bool> canEdit(const CanEditInput& input) const;

  ProjectRepository repository_;
  ProjectMemberRepository memberRepository_;
};
