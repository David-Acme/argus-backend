#include "project-task-feature-service.hxx"

#include <ctime>
#include <trantor/utils/Logger.h>

drogon::Task<bool> ProjectTaskFeatureService::canWorkOn(int64_t projectId,
                                                       int64_t actorId) const
{
  const auto project = co_await projectRepository_.findById(projectId);
  if (!project)
    co_return false;
  if (project->ownerId == actorId)
    co_return true;
  const auto access = co_await memberRepository_.findAccess(projectId, actorId);
  co_return access && *access == ShareAccess::Edit;
}

drogon::Task<void>
ProjectTaskFeatureService::emit(SyncOperation operation,
                                const ProjectTaskSchema& row) const
{
  const auto project = co_await projectRepository_.findById(row.projectId);
  if (!project)
    co_return;

  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::ProjectTask;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }

  auto recipients = co_await memberRepository_.memberIds(row.projectId);
  recipients.push_back(project->ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project task emit";
    co_return;
  }
  sink->emitUsers(recipients, body);
  co_return;
}

drogon::Task<std::optional<ProjectTaskSchema>>
ProjectTaskFeatureService::create(const CreateProjectTaskDto& body,
                                  int64_t actorId) const
{
  if (!co_await canWorkOn(body.projectId, actorId))
    co_return std::nullopt;

  const auto row = co_await repository_.create({
      .projectId = body.projectId,
      .createdBy = actorId > 0 ? std::optional<int64_t>(actorId) : std::nullopt,
      .assigneeId = body.assigneeId,
      .title = body.title,
      .status = body.status,
      .priority = body.priority,
      .dueAt = body.dueAt,
      .sortOrder = body.sortOrder,
  });
  co_await emit(SyncOperation::Add, row);
  co_return row;
}

drogon::Task<std::optional<ProjectTaskSchema>>
ProjectTaskFeatureService::update(const UpdateInput& input) const
{
  const auto existing = co_await repository_.findById(input.id);
  if (!existing)
    co_return std::nullopt;
  if (!co_await canWorkOn(existing->projectId, input.actorId))
    co_return std::nullopt;

  const auto row = co_await repository_.update(input.id, {
      .title = input.body.title,
      .status = input.body.status,
      .priority = input.body.priority,
      .assigneeId = input.body.assigneeId,
      .dueAt = input.body.dueAt,
      .sortOrder = input.body.sortOrder,
  });
  if (row.id == 0)
    co_return std::nullopt;
  const auto project = co_await projectRepository_.findById(row.projectId);
  if (project) {
    auto recipients = co_await memberRepository_.memberIds(row.projectId);
    recipients.push_back(project->ownerId);
    if (const auto* sink = user_change::getProductivitySink())
      co_await sink->publishAudit(UserAuditInput{
          .recordId = row.id,
          .tableName = TableName::ProjectTask,
          .before = existing->toJson(),
          .after = row.toJson(),
          .userIds = std::move(recipients),
      });
    else
      LOG_WARN << "user change sink not installed; drop project task audit";
  }
  co_return row;
}

drogon::Task<bool> ProjectTaskFeatureService::remove(int64_t id,
                                                      int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return false;
  if (!co_await canWorkOn(existing->projectId, actorId))
    co_return false;

  const bool removed = co_await repository_.remove(id);
  if (removed)
    co_await emit(SyncOperation::Delete, *existing);
  co_return removed;
}
