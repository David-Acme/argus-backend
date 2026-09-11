#include "synchronized-service.hxx"

#include <config/app-config.hxx>
#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <stdexcept>

namespace
{
std::optional<CameraSyncTable> cameraSyncTableFor(TableName table)
{
  switch (table) {
    case TableName::Camera:
      return CameraSyncTable::Camera;
    case TableName::CameraStream:
      return CameraSyncTable::CameraStream;
    case TableName::Zone:
      return CameraSyncTable::Zone;
    default:
      return std::nullopt;
  }
}

std::optional<ProductivitySyncTable> productivitySyncTableFor(TableName table)
{
  switch (table) {
    case TableName::Reminder:
      return ProductivitySyncTable::Reminder;
    case TableName::ReminderDetail:
      return ProductivitySyncTable::ReminderDetail;
    case TableName::CalendarEvent:
      return ProductivitySyncTable::CalendarEvent;
    case TableName::CalendarEventShare:
      return ProductivitySyncTable::CalendarEventShare;
    case TableName::Project:
      return ProductivitySyncTable::Project;
    case TableName::ProjectMember:
      return ProductivitySyncTable::ProjectMember;
    case TableName::ProjectTask:
      return ProductivitySyncTable::ProjectTask;
    default:
      return std::nullopt;
  }
}
} // namespace

const Syncable& SynchronizedService::repoFor(TableName table) const
{
  switch (table) {
    case TableName::User:
      return userRepository_;
    case TableName::UserInvitation:
      return userInvitationRepository_;
    case TableName::Event:
      return eventRepository_;
    case TableName::Person:
      return personRepository_;
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
    if (range->startId)
      filter.startId = *range->startId;
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
    if (!rows.empty()) {
      const auto& last = rows.back();
      node["lastSyncDate"]["createdId"] = last.get("id", Json::Value());
      node["lastSyncDate"]["created"] = last.get("createdAt", Json::Value());
    }
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
    if (!rows.empty()) {
      const auto& last = rows.back();
      node["lastSyncDate"]["deletedId"] = last.get("id", Json::Value());
      node["lastSyncDate"]["deleted"] = last.get("deletedAt", Json::Value());
    }
  }
  else {
    node["deleted"] = Json::arrayValue;
  }

  if (dto.findLastCreated || dto.findLastDeleted) {
    Json::Value last(Json::objectValue);
    if (dto.findLastCreated) {
      const auto v = co_await repo.findLast(base);
      if (v) {
        if ((*v).isMember("id"))
          last["createdId"] = (*v)["id"];
        if ((*v).isMember("createdAt"))
          last["created"] = (*v)["createdAt"];
      }
    }
    if (dto.findLastDeleted) {
      const auto v = co_await repo.findLastDeleted(base);
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
    const SynchronizedBodyDto& dto, const JwtContext& ctx) const
{
  if (!notificationSyncSource_)
    throw ResponseException(
        {.message = "Notification sync unavailable",
         .statusCode = 503,
         .errorCode = AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});

  Json::Value node(Json::objectValue);

  if (dto.requiredCreate) {
    SyncFilter filter;
    if (dto.created) {
      filter.startTime = dto.created->startTime;
      filter.startId = dto.created->startId;
      filter.endTime = dto.created->endTime;
    }
    const auto rows = co_await notificationSyncSource_->find(ctx, filter);
    Json::Value arr(Json::arrayValue);
    for (const auto& row : rows)
      arr.append(row);
    node["created"] = arr;
    if (!rows.empty()) {
      const auto& last = rows.back();
      node["lastSyncDate"]["createdId"] = last.get("id", Json::Value());
      node["lastSyncDate"]["created"] = last.get("createdAt", Json::Value());
    }
  }
  else {
    node["created"] = Json::arrayValue;
  }
  node["deleted"] = Json::arrayValue;

  if (dto.findLastCreated) {
    const auto v = co_await notificationSyncSource_->findLast(ctx);
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
      {"user_invitation", &SynchronizedDto::userInvitation},
      {"camera", &SynchronizedDto::camera},
      {"camera_stream", &SynchronizedDto::cameraStream},
      {"zone", &SynchronizedDto::zone},
      {"reminder", &SynchronizedDto::reminder},
      {"reminder_detail", &SynchronizedDto::reminderDetail},
      {"calendar_event", &SynchronizedDto::calendarEvent},
      {"calendar_event_share", &SynchronizedDto::calendarEventShare},
      {"project", &SynchronizedDto::project},
      {"project_member", &SynchronizedDto::projectMember},
      {"project_task", &SynchronizedDto::projectTask},
      {"event", &SynchronizedDto::event},
      {"person", &SynchronizedDto::person},
      {"notification", &SynchronizedDto::notification},
  };

  Json::Value out(Json::objectValue);
  for (const auto& [name, member] : kBodyFields) {
    if (!(body.*member))
      continue;

    const auto table = tableNameFromString(name);
    if (!role_access::hasAccess(
            {.role = ctx.role, .table = table, .perm = RolePermission::Read})) {
      out[name] = Json::nullValue;
      continue;
    }

    if (table == TableName::Notification) {
      out[name] = co_await syncUserNotification(*(body.*member), ctx);
      continue;
    }

    if (const auto cameraTable = cameraSyncTableFor(table)) {
      if (!cameraSyncSource_ || !cameraSyncSource_->serves(*cameraTable))
        throw ResponseException(
            {.message = "Camera sync unavailable",
             .statusCode = 503,
             .errorCode = AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});
      const auto source = cameraSyncSource_->sourceFor(*cameraTable, ctx);
      out[name] =
          co_await syncWithRepo({.repo = *source, .dto = *(body.*member)}, {});
      continue;
    }

    if (const auto productivityTable = productivitySyncTableFor(table)) {
      if (!productivitySyncSource_ ||
          !productivitySyncSource_->serves(*productivityTable))
        throw ResponseException(
            {.message = "Productivity sync unavailable",
             .statusCode = 503,
             .errorCode = AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});
      const auto source =
          productivitySyncSource_->sourceFor(*productivityTable, ctx);
      out[name] =
          co_await syncWithRepo({.repo = *source, .dto = *(body.*member)}, {});
      continue;
    }

    const auto& repo = repoFor(table);
    SyncFilter base{};
    if (table == TableName::User && ctx.role != UserRole::Owner &&
        ctx.role != UserRole::Guard)
      base.userId = ctx.sub;
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
  if (body.afterId)
    filter.afterId = *body.afterId;
  if (body.endId)
    filter.endId = *body.endId;
  if (body.startTime)
    filter.startTime = *body.startTime;
  if (body.endTime)
    filter.endTime = *body.endTime;

  Json::Value out(Json::objectValue);
  std::vector<Json::Value> rows;
  if (body.findLast && !body.afterId)
    rows = std::vector<Json::Value>{};
  else
    rows = co_await auditLogRepository_.findSync(filter);
  Json::Value arr(Json::arrayValue);
  for (const auto& row : rows)
    arr.append(row);
  out["info"] = arr;
  if (!rows.empty())
    out["nextCursorId"] = rows.back().get("id", Json::Value());

  if (body.findLast) {
    const auto last = co_await auditLogRepository_.findLastSync(filter);
    Json::Value record;
    if (last) {
      record["id"] = (*last).get("id", Json::Value());
      record["lastSyncDate"] = (*last).get("eventTimestamp", Json::Value());
      out["watermarkId"] = (*last).get("id", Json::Value());
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
  if (body.afterId)
    filter.afterId = *body.afterId;
  if (body.endId)
    filter.endId = *body.endId;
  if (body.startTime)
    filter.startTime = *body.startTime;
  if (body.endTime)
    filter.endTime = *body.endTime;

  Json::Value out(Json::objectValue);
  std::vector<Json::Value> rows;
  if (body.findLast && !body.afterId)
    rows = std::vector<Json::Value>{};
  else
    rows = co_await userAuditLogRepository_.findSync(filter);
  Json::Value arr(Json::arrayValue);
  for (const auto& row : rows)
    arr.append(row);
  out["info"] = arr;
  if (!rows.empty())
    out["nextCursorId"] = rows.back().get("id", Json::Value());

  if (body.findLast) {
    const auto last = co_await userAuditLogRepository_.findLastSync(filter);
    Json::Value record;
    if (last) {
      record["id"] = (*last).get("id", Json::Value());
      record["lastSyncDate"] = (*last).get("eventTimestamp", Json::Value());
      out["watermarkId"] = (*last).get("id", Json::Value());
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
