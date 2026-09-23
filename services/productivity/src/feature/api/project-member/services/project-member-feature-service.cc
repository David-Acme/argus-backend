#include "project-member-feature-service.hxx"

#include <ctime>
#include <errors/response-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>
#include <auth/role-access.hxx>
#include <productivity/membership-error.hxx>

#include <vector>

drogon::Task<void> ProjectMemberFeatureService::emitMembership(
    const EmitMembershipInput& input) const
{
  const SyncOperation operation = input.operation;
  const ProjectMemberSchema& row = input.row;
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::ProjectMember;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project member membership emit";
    co_return;
  }
  std::vector<int64_t> recipients{input.ownerId, row.userId};
  co_await sink->emitUsers({.userIds = std::move(recipients),
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<void> ProjectMemberFeatureService::emitParent(
    const EmitParentInput& input) const
{
  const SyncOperation operation = input.operation;
  const auto parent =
      co_await parentRepository_.findById(input.parentId, input.client);
  if (!parent)
    co_return;

  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Project;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = parent->id;
    tombstone["deletedAt"] = static_cast<Json::Int64>(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = parent->toJson();
  }
  const auto* sink = user_change::getProductivitySink();
  if (!sink) {
    LOG_WARN << "user change sink not installed; drop project member parent emit";
    co_return;
  }
  co_await sink->emitUsers({.userIds = {input.userId},
                            .body = std::move(body),
                            .client = input.client});
  co_return;
}

drogon::Task<ProjectMemberResult>
ProjectMemberFeatureService::create(const CreateProjectMemberDto& body, int64_t actorId) const
{
  const auto parent = co_await parentRepository_.findById(body.projectId);
  if (!parent || parent->ownerId != actorId)
    co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
  if (body.userId == parent->ownerId)
    co_return {.error = MembershipError::SelfShare, .row = std::nullopt};

  const auto target = co_await directory_.findById(body.userId);
  if (!target || !target->isActive)
    co_return {.error = MembershipError::UserNotFound, .row = std::nullopt};
  if (!role_access::hasAccess({.role = target->role,
                               .table = TableName::Project,
                               .perm = RolePermission::Read}))
    co_return {.error = MembershipError::UserNotAllowed, .row = std::nullopt};

  const auto access = shareAccessFromString(body.access);
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectMemberSchema row;
  try {
    if (const auto existing = co_await repository_.findExisting(
            {.parentId = body.projectId,
             .userId = body.userId,
             .client = transaction.get()})) {
      row = co_await repository_.updateAccess(
          {.id = existing->id, .access = access, .client = transaction.get()});
      if (row.id == 0) {
        db_transaction::rollback(transaction);
        co_return {.error = MembershipError::ParentNotFound,
                   .row = std::nullopt};
      }
      const auto* sink = user_change::getProductivitySink();
      if (!sink)
        LOG_WARN << "user change sink not installed; drop project member audit";
      else
        co_await sink->publishAudit({.recordId = row.id,
                                     .tableName = TableName::ProjectMember,
                                     .before = existing->toJson(),
                                     .after = row.toJson(),
                                     .userIds = {parent->ownerId, row.userId},
                                     .client = transaction.get()});
    }
    else {
      row = co_await repository_.create({.projectId = body.projectId,
                                         .userId = body.userId,
                                         .access = access,
                                         .client = transaction.get()});
      co_await emitMembership({.operation = SyncOperation::Add,
                               .row = row,
                               .ownerId = parent->ownerId,
                               .client = transaction.get()});
      co_await emitParent({.operation = SyncOperation::Add,
                           .parentId = body.projectId,
                           .userId = body.userId,
                           .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return {.row = row};
}

drogon::Task<ProjectMemberResult>
ProjectMemberFeatureService::update(const UpdateInput& input) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectMemberSchema row;
  Json::Value before;
  try {
    const auto existing =
        co_await repository_.findById(input.id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
    }

    const auto parent = co_await parentRepository_.findById(
        existing->projectId, transaction.get());
    if (!parent || parent->ownerId != input.actorId) {
      db_transaction::rollback(transaction);
      co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
    }
    before = existing->toJson();

    row = co_await repository_.updateAccess(
        {.id = input.id,
         .access = shareAccessFromString(input.body.access),
         .client = transaction.get()});
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return {.error = MembershipError::ParentNotFound, .row = std::nullopt};
    }

    const auto* sink = user_change::getProductivitySink();
    if (!sink)
      LOG_WARN << "user change sink not installed; drop project member audit";
    else
      co_await sink->publishAudit({.recordId = row.id,
                                   .tableName = TableName::ProjectMember,
                                   .before = std::move(before),
                                   .after = row.toJson(),
                                   .userIds = {parent->ownerId, row.userId},
                                   .client = transaction.get()});

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return {.row = row};
}

drogon::Task<bool> ProjectMemberFeatureService::remove(int64_t id,
                                                int64_t actorId) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::productivityClient());
  ProjectMemberSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    const auto parent = co_await parentRepository_.findById(
        existing->projectId, transaction.get());
    if (!parent || parent->ownerId != actorId) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    before = *existing;

    if (!co_await repository_.remove(id, transaction.get())) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    co_await emitMembership({.operation = SyncOperation::Delete,
                             .row = before,
                             .ownerId = parent->ownerId,
                             .client = transaction.get()});
    co_await emitParent({.operation = SyncOperation::Delete,
                         .parentId = existing->projectId,
                         .userId = existing->userId,
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
