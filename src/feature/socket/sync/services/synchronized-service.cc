#include "synchronized-service.hxx"

#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <stdexcept>

const Syncable& SynchronizedService::repoFor(TableName table) const
{
  switch (table) {
    case TableName::User:
      return userRepository_;
    case TableName::Camera:
      return cameraRepository_;
    case TableName::CameraStream:
      return cameraStreamRepository_;
    case TableName::Zone:
      return zoneRepository_;
    case TableName::Reminder:
      return reminderRepository_;
    case TableName::ReminderDetail:
      return reminderDetailRepository_;
    default:
      throw std::invalid_argument("table is not syncable");
  }
}

SyncFilter
SynchronizedService::applyRange(const SyncFilter& base,
                                const std::optional<SynchronizedRangeDto>& range) const
{
  SyncFilter filter = base;
  if (range) {
    if (range->startTime)
      filter.startTime = *range->startTime;
    if (range->endTime)
      filter.endTime = *range->endTime;
  }
  return filter;
}

drogon::Task<Json::Value> SynchronizedService::syncWithRepo(
    const SyncWithRepoInput& input, const SyncFilter& base) const
{
  const auto& repo = input.repo;
  const auto& dto = input.dto;
  Json::Value node(Json::objectValue);

  if (dto.requiredCreate) {
    const SyncFilter filter = applyRange(base, dto.created);
    const auto rows = co_await repo.find(filter);
    Json::Value arr(Json::arrayValue);
    for (const auto& row : rows)
      arr.append(row);
    node["created"] = arr;
  }
  else {
    node["created"] = Json::arrayValue;
  }

  if (dto.requiredDeleted) {
    const SyncFilter filter = applyRange(base, dto.deleted);
    const auto rows = co_await repo.findDeleted(filter);
    Json::Value darr(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value record;
      record["id"] = row.get("id", Json::Value());
      record["deletedAt"] = row.get("deletedAt", Json::Value());
      darr.append(record);
    }
    node["deleted"] = darr;
  }
  else {
    node["deleted"] = Json::arrayValue;
  }

  if (dto.findLastCreated || dto.findLastDeleted) {
    Json::Value last(Json::objectValue);
    if (dto.findLastCreated) {
      const auto v = co_await repo.findLast();
      if (v) {
        if ((*v).isMember("id"))
          last["createdId"] = (*v)["id"];
        if ((*v).isMember("createdAt"))
          last["created"] = (*v)["createdAt"];
      }
    }
    if (dto.findLastDeleted) {
      const auto v = co_await repo.findLastDeleted();
      if (v) {
        if ((*v).isMember("id"))
          last["deletedId"] = (*v)["id"];
        if ((*v).isMember("deletedAt"))
          last["deleted"] = (*v)["deletedAt"];
      }
    }
    node["lastSyncDate"] = last;
  }

  co_return node;
}

drogon::Task<Json::Value> SynchronizedService::syncUserNotification(
    const SynchronizedBodyDto& dto, int64_t userId) const
{
  Json::Value node(Json::objectValue);

  if (dto.requiredCreate) {
    NotificationSyncFilter filter;
    filter.userId = userId;
    if (dto.created) {
      if (dto.created->startTime)
        filter.startTime = *dto.created->startTime;
      if (dto.created->endTime)
        filter.endTime = *dto.created->endTime;
    }
    const auto rows = co_await notificationRepository_.findSync(filter);
    Json::Value arr(Json::arrayValue);
    for (const auto& row : rows)
      arr.append(row);
    node["created"] = arr;
  }
  else {
    node["created"] = Json::arrayValue;
  }
  node["deleted"] = Json::arrayValue;

  if (dto.findLastCreated) {
    NotificationSyncFilter filter;
    filter.userId = userId;
    const auto v = co_await notificationRepository_.findLastSync(filter);
    Json::Value last(Json::objectValue);
    if (v) {
      if ((*v).isMember("id"))
        last["createdId"] = (*v)["id"];
      if ((*v).isMember("createdAt"))
        last["created"] = (*v)["createdAt"];
    }
    node["lastSyncDate"] = last;
  }

  co_return node;
}

std::vector<TableName> SynchronizedService::auditTablesForRole(UserRole role) const
{
  static const std::unordered_set<TableName> kExcluded = {
      TableName::AuditLog, TableName::UserAuditLog, TableName::Notification,
      TableName::NotificationToken, TableName::RefreshToken,
      TableName::FaceEmbedding, TableName::PersonEvent,
  };

  std::vector<TableName> tables;
  for (const auto table : role_access::readableTables(role)) {
    if (!kExcluded.contains(table))
      tables.push_back(table);
  }
  return tables;
}

drogon::Task<Json::Value> SynchronizedService::sync(const SynchronizedDto& body,
                                                    const JwtContext& ctx) const
{
  using BodyField = std::optional<SynchronizedBodyDto> SynchronizedDto::*;
  static const std::vector<std::pair<std::string, BodyField>> kBodyFields = {
      {"user", &SynchronizedDto::user},
      {"camera", &SynchronizedDto::camera},
      {"camera_stream", &SynchronizedDto::cameraStream},
      {"zone", &SynchronizedDto::zone},
      {"reminder", &SynchronizedDto::reminder},
      {"reminder_detail", &SynchronizedDto::reminderDetail},
      {"notification", &SynchronizedDto::notification},
  };

  Json::Value out(Json::objectValue);
  for (const auto& [name, member] : kBodyFields) {
    if (!(body.*member))
      continue;

    const auto table = tableNameFromString(name);
    if (!role_access::hasAccess(ctx.role, table, RolePermission::Read)) {
      out[name] = Json::nullValue;
      continue;
    }

    if (table == TableName::Notification) {
      out[name] = co_await syncUserNotification(*(body.*member), ctx.sub);
      continue;
    }

    const auto& repo = repoFor(table);
    const SyncFilter base{};
    out[name] =
        co_await syncWithRepo({.repo = repo, .dto = *(body.*member)}, base);
  }

  SocketEmitDto response;
  response.operation = SyncOperation::Synchronize;
  response.obj = out;
  co_return response.toJson();
}

drogon::Task<Json::Value>
SynchronizedService::syncAuditLog(const SynchronizedLogDto& body,
                                  const JwtContext& ctx) const
{
  AuditLogSyncFilter filter;
  filter.tableNames = auditTablesForRole(ctx.role);
  if (body.startTime)
    filter.startTime = *body.startTime;
  if (body.endTime)
    filter.endTime = *body.endTime;

  Json::Value out(Json::objectValue);
  const auto rows = co_await auditLogRepository_.findSync(filter);
  Json::Value arr(Json::arrayValue);
  for (const auto& row : rows)
    arr.append(row);
  out["info"] = arr;

  if (body.findLast) {
    const auto last = co_await auditLogRepository_.findLastSync(filter);
    Json::Value record;
    if (last) {
      record["id"] = (*last).get("id", Json::Value());
      record["lastSyncDate"] = (*last).get("eventTimestamp", Json::Value());
    }
    else {
      record = Json::nullValue;
    }
    out["lastSyncRecord"] = record;
  }

  SocketEmitDto response;
  response.operation = SyncOperation::SynchronizeAuditLog;
  response.obj = out;
  co_return response.toJson();
}

drogon::Task<Json::Value>
SynchronizedService::syncUserAuditLog(const SynchronizedLogDto& body,
                                      const JwtContext& ctx) const
{
  UserAuditLogSyncFilter filter;
  filter.userId = ctx.sub;
  if (body.startTime)
    filter.startTime = *body.startTime;
  if (body.endTime)
    filter.endTime = *body.endTime;

  Json::Value out(Json::objectValue);
  const auto rows = co_await userAuditLogRepository_.findSync(filter);
  Json::Value arr(Json::arrayValue);
  for (const auto& row : rows)
    arr.append(row);
  out["info"] = arr;

  if (body.findLast) {
    const auto last = co_await userAuditLogRepository_.findLastSync(filter);
    Json::Value record;
    if (last) {
      record["id"] = (*last).get("id", Json::Value());
      record["lastSyncDate"] = (*last).get("eventTimestamp", Json::Value());
    }
    else {
      record = Json::nullValue;
    }
    out["lastSyncRecord"] = record;
  }

  SocketEmitDto response;
  response.operation = SyncOperation::SynchronizeUserAuditLog;
  response.obj = out;
  co_return response.toJson();
}
