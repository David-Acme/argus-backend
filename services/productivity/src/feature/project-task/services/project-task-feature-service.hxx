#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/project-task/dtos/create-project-task-dto.hxx>
#include <feature/project-task/dtos/update-project-task-dto.hxx>
#include <optional>
#include <shared/repositories/project-task/project-task-repository.hxx>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/schemas/project-task/project-task-schema.hxx>
#include <sync/user-change-sink.hxx>

class ProjectTaskFeatureService
{
public:
  struct UpdateInput
  {
    int64_t id{0};
    const UpdateProjectTaskDto& body;
    int64_t actorId{0};
  };

  drogon::Task<std::optional<ProjectTaskSchema>>
  create(const CreateProjectTaskDto& body, int64_t actorId) const;
  drogon::Task<std::optional<ProjectTaskSchema>>
  update(const UpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  struct EmitInput
  {
    SyncOperation operation{};
    const ProjectTaskSchema& row;
    drogon::orm::DbClient* client{nullptr};
  };

  struct CanWorkOnInput
  {
    int64_t projectId{0};
    int64_t actorId{0};
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<bool> canWorkOn(const CanWorkOnInput& input) const;
  drogon::Task<void> emit(const EmitInput& input) const;

  ProjectTaskRepository repository_;
  ProjectRepository projectRepository_;
  ProjectMemberRepository memberRepository_;
};
