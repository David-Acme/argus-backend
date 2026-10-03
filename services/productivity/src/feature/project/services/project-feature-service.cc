#include "project-feature-service.hxx"

#include <ctime>
#include <errors/response-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<void> ProjectFeatureService::emit(const EmitInput& input) const
{
  const SyncOperation operation = input.operation;
  const ProjectSchema& row = input.row;
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

  auto recipients = co_await memberRepository_.memberIds(row.id, input.client);
  recipients.push_back(row.ownerId);
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project emit";
    co_return;
  }
  co_await sink->emitUsers({.userIds = std::move(recipients),
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<void>
ProjectFeatureService::retireTasks(const EmitInput& input) const
{
  const ProjectSchema& project = input.row;
  const auto tasks =
      co_await taskRepository_.findByProject(project.id, input.client);
  if (tasks.empty())
    co_return;
  co_await taskRepository_.removeByProject(project.id, input.client);

  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project task tombstones";
    co_return;
  }
  auto recipients = co_await memberRepository_.memberIds(project.id, input.client);
  recipients.push_back(project.ownerId);
  const auto deletedAt = static_cast<Json::Int64>(std::time(nullptr));
  for (const auto& task : tasks) {
    SocketEmitDto body;
    body.operation = SyncOperation::Delete;
    body.option = TableName::ProjectTask;
    Json::Value tombstone;
    tombstone["id"] = task.id;
    tombstone["deletedAt"] = deletedAt;
    body.obj = tombstone;
    co_await sink->emitUsers({.userIds = recipients,
                              .body = std::move(body),
                              .client = input.client});
  }
}

drogon::Task<bool>
ProjectFeatureService::canEdit(const CanEditInput& input) const
{
  if (input.row.ownerId == input.actorId)
    co_return true;
  const auto access = co_await memberRepository_.findAccess(
      {.parentId = input.row.id, .userId = input.actorId, .client = input.client});
  co_return access && *access == ShareAccess::Edit;
}

drogon::Task<ProjectSchema>
ProjectFeatureService::create(const CreateProjectDto& body,
                              int64_t ownerId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectSchema row;
  try {
    row = co_await repository_.create({
        .ownerId = ownerId,
        .name = body.name,
        .description = body.description,
        .status = body.status,
        .color = body.color,
        .startsAt = body.startsAt,
        .targetAt = body.targetAt,
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

drogon::Task<std::optional<ProjectSchema>>
ProjectFeatureService::update(const UpdateInput& input) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectSchema row;
  Json::Value before;
  try {
    const auto existing =
        co_await repository_.findById(input.id, transaction.get());
    if (!existing ||
        !co_await canEdit({.row = *existing,
                           .actorId = input.actorId,
                           .client = transaction.get()})) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    before = existing->toJson();

    row = co_await repository_.update(input.id, {
        .name = input.body.name,
        .description = input.body.description,
        .status = input.body.status,
        .color = input.body.color,
        .startsAt = input.body.startsAt,
        .targetAt = input.body.targetAt,
        .client = transaction.get(),
    });
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    auto recipients =
        co_await memberRepository_.memberIds(row.id, transaction.get());
    recipients.push_back(row.ownerId);
    const auto* sink = user_change::getProductivitySink();
    if (!sink)
      LOG_WARN << "user change sink not installed; drop project audit";
    else
      co_await sink->publishAudit({.recordId = row.id,
                                   .tableName = TableName::Project,
                                   .before = std::move(before),
                                   .after = row.toJson(),
                                   .userIds = std::move(recipients),
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

drogon::Task<bool> ProjectFeatureService::remove(int64_t id,
                                                 int64_t actorId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing || existing->ownerId != actorId) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    before = *existing;

    if (!co_await repository_.remove(id, transaction.get())) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    co_await retireTasks({.operation = SyncOperation::Delete,
                          .row = before,
                          .client = transaction.get()});
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
