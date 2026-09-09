#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/project-task/dtos/create-project-task-dto.hxx>
#include <feature/api/project-task/dtos/update-project-task-dto.hxx>
#include <optional>
#include <shared/repositories/project-task/project-task-repository.hxx>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/schemas/project-task/project-task-schema.hxx>
#include <shared/contracts/user-change-sink.hxx>

class ProjectTaskFeatureService
{
public:
  drogon::Task<std::optional<ProjectTaskSchema>>
  create(const CreateProjectTaskDto& body, int64_t actorId) const;
  drogon::Task<std::optional<ProjectTaskSchema>>
  update(int64_t id, const UpdateProjectTaskDto& body, int64_t actorId) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  // Permission check and emit targets both resolve through the parent project.
  drogon::Task<bool> canWorkOn(int64_t projectId, int64_t actorId) const;
  drogon::Task<void> emit(SyncOperation operation,
                          const ProjectTaskSchema& row) const;

  ProjectTaskRepository repository_;
  ProjectRepository projectRepository_;
  ProjectMemberRepository memberRepository_;
};
