#include "project-feature-service.hxx"

#include <ctime>
#include <trantor/utils/Logger.h>

drogon::Task<void> ProjectFeatureService::emit(SyncOperation operation,
                                              const ProjectSchema& row) const
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Project;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }

  auto recipients = co_await memberRepository_.memberIds(row.id);
  recipients.push_back(row.ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project emit";
    co_return;
  }
  sink->emitUsers(recipients, body);
  co_return;
}

drogon::Task<bool> ProjectFeatureService::canEdit(const ProjectSchema& row,
                                                 int64_t actorId) const
{
  if (row.ownerId == actorId)
    co_return true;
  const auto access = co_await memberRepository_.findAccess(row.id, actorId);
  co_return access && *access == ShareAccess::Edit;
}

drogon::Task<ProjectSchema>
ProjectFeatureService::create(const CreateProjectDto& body,
                              int64_t ownerId) const
{
  const auto row = co_await repository_.create({
      .ownerId = ownerId,
      .name = body.name,
      .description = body.description,
      .status = body.status,
      .color = body.color,
      .startsAt = body.startsAt,
      .targetAt = body.targetAt,
  });
  co_await emit(SyncOperation::Add, row);
  co_return row;
}

drogon::Task<std::optional<ProjectSchema>>
ProjectFeatureService::update(int64_t id, const UpdateProjectDto& body,
                              int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  // The owner always; a member only when their membership grants `edit`.
  if (!existing || !co_await canEdit(*existing, actorId))
    co_return std::nullopt;

  const auto row = co_await repository_.update(id, {
      .name = body.name,
      .description = body.description,
      .status = body.status,
      .color = body.color,
      .startsAt = body.startsAt,
      .targetAt = body.targetAt,
  });
  if (row.id == 0)
    co_return std::nullopt;
  auto recipients = co_await memberRepository_.memberIds(row.id);
  recipients.push_back(row.ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (sink)
    co_await sink->publishAudit(UserAuditInput{
        .recordId = row.id,
        .tableName = TableName::Project,
        .before = existing->toJson(),
        .after = row.toJson(),
        .userIds = std::move(recipients),
    });
  else
    LOG_WARN << "user change sink not installed; drop project audit";
  co_return row;
}

drogon::Task<bool> ProjectFeatureService::remove(int64_t id,
                                                  int64_t actorId) const
{
  const auto existing = co_await repository_.findById(id);
  // Deleting the project is the owner's alone; a member with `edit` works
  // inside it but cannot remove it from the owner's list.
  if (!existing || existing->ownerId != actorId)
    co_return false;

  const bool removed = co_await repository_.remove(id);
  if (removed)
    co_await emit(SyncOperation::Delete, *existing);
  co_return removed;
}
