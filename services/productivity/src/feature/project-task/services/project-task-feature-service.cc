#include "project-task-feature-service.hxx"

#include <ctime>
#include <errors/response-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<bool>
ProjectTaskFeatureService::canWorkOn(const CanWorkOnInput& input) const
{
  const auto project =
      co_await projectRepository_.findById(input.projectId, input.client);
  if (!project)
    co_return false;
  if (project->ownerId == input.actorId)
    co_return true;
  const auto access = co_await memberRepository_.findAccess(
      {.parentId = input.projectId,
       .userId = input.actorId,
       .client = input.client});
  co_return access && *access == ShareAccess::Edit;
}

drogon::Task<void>
ProjectTaskFeatureService::emit(const EmitInput& input) const
{
  const SyncOperation operation = input.operation;
  const ProjectTaskSchema& row = input.row;
  const auto project =
      co_await projectRepository_.findById(row.projectId, input.client);
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

  auto recipients =
      co_await memberRepository_.memberIds(row.projectId, input.client);
  recipients.push_back(project->ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project task emit";
    co_return;
  }
  co_await sink->emitUsers({.userIds = std::move(recipients),
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<std::optional<ProjectTaskSchema>>
ProjectTaskFeatureService::create(const CreateProjectTaskDto& body,
                                  int64_t actorId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectTaskSchema row;
  try {
    if (!co_await canWorkOn({.projectId = body.projectId,
                             .actorId = actorId,
                             .client = transaction.get()})) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    row = co_await repository_.create({
        .projectId = body.projectId,
        .createdBy = actorId > 0 ? std::optional<int64_t>(actorId) : std::nullopt,
        .assigneeId = body.assigneeId,
        .title = body.title,
        .status = body.status,
        .priority = body.priority,
        .dueAt = body.dueAt,
        .sortOrder = body.sortOrder,
        .client = transaction.get(),
    });
    co_await emit({.operation = SyncOperation::Add,
                   .row = row,
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<std::optional<ProjectTaskSchema>>
ProjectTaskFeatureService::update(const UpdateInput& input) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectTaskSchema row;
  Json::Value before;
  try {
    const auto existing =
        co_await repository_.findById(input.id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    if (!co_await canWorkOn({.projectId = existing->projectId,
                             .actorId = input.actorId,
                             .client = transaction.get()})) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    before = existing->toJson();

    row = co_await repository_.update(input.id, {
        .title = input.body.title,
        .status = input.body.status,
        .priority = input.body.priority,
        .assigneeId = input.body.assigneeId,
        .dueAt = input.body.dueAt,
        .sortOrder = input.body.sortOrder,
        .client = transaction.get(),
    });
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    const auto project =
        co_await projectRepository_.findById(row.projectId, transaction.get());
    if (project) {
      auto recipients =
          co_await memberRepository_.memberIds(row.projectId, transaction.get());
      recipients.push_back(project->ownerId);
      if (const auto* sink = user_change::getProductivitySink())
        co_await sink->publishAudit({.recordId = row.id,
                                     .tableName = TableName::ProjectTask,
                                     .before = std::move(before),
                                     .after = row.toJson(),
                                     .userIds = std::move(recipients),
                                     .client = transaction.get()});
      else
        LOG_WARN << "user change sink not installed; drop project task audit";
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<bool> ProjectTaskFeatureService::remove(int64_t id,
                                                     int64_t actorId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectTaskSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    if (!co_await canWorkOn({.projectId = existing->projectId,
                             .actorId = actorId,
                             .client = transaction.get()})) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    before = *existing;

    if (!co_await repository_.remove(id, transaction.get())) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    co_await emit({.operation = SyncOperation::Delete,
                   .row = before,
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return true;
}
