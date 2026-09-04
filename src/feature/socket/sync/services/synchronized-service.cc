#include "synchronized-service.hxx"

#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <stdexcept>

namespace
{
/** Tables whose rows belong to one user plus the people they shared with. */
bool isPersonalTable(TableName table)
{
  switch (table) {
    case TableName::CalendarEvent:
    case TableName::CalendarEventShare:
    case TableName::Project:
    case TableName::ProjectMember:
    case TableName::ProjectTask:
      return true;
    default:
      return false;
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
    case TableName::CalendarEvent:
      return calendarEventRepository_;
    case TableName::CalendarEventShare:
      return calendarEventShareRepository_;
    case TableName::Project:
      return projectRepository_;
    case TableName::ProjectMember:
      return projectMemberRepository_;
    case TableName::ProjectTask:
      return projectTaskRepository_;
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
    const SynchronizedBodyDto& dto, int64_t userId) const
{
  Json::Value node(Json::objectValue);

  if (dto.requiredCreate) {
    NotificationSyncFilter filter;
    filter.userId = userId;
    if (dto.created) {
      if (dto.created->startTime)
        filter.startTime = *dto.created->startTime;
      if (dto.created->startId)
        filter.startId = *dto.created->startId;
      if (dto.created->endTime)
        filter.endTime = *dto.created->endTime;
    }
    const auto rows = co_await notificationRepository_.findSync(filter);
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
    if (!role_access::hasAccess(ctx.role, table, RolePermission::Read)) {
      out[name] = Json::nullValue;
      continue;
    }

    if (table == TableName::Notification) {
      out[name] = co_await syncUserNotification(*(body.*member), ctx.sub);
      continue;
    }

    const auto& repo = repoFor(table);
    // A personal table is scoped to the caller whatever their role: the role
    // decides which tables exist for them, the scope decides which rows.
    SyncFilter base{};
    if (isPersonalTable(table))
      base.userId = ctx.sub;
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
