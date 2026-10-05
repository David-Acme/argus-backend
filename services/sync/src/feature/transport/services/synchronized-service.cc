#include "synchronized-service.hxx"

#include <errors/response-exception.hxx>
#include <auth/role-access.hxx>
#include <sync/sync-operation.hxx>
#include <sync/sync-errors.hxx>
#include <trantor/net/EventLoop.h>

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

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
std::optional<IdentitySyncTable> identitySyncTableFor(TableName table)
{
  switch (table) {
    case TableName::User:
      return IdentitySyncTable::User;
    case TableName::UserInvitation:
      return IdentitySyncTable::UserInvitation;
    case TableName::Person:
      return IdentitySyncTable::Person;
    default:
      return std::nullopt;
  }
}

constexpr std::size_t kPullWidth = 4;

struct PullJoin
{
  std::mutex mutex;
  std::size_t pending{0};
  std::coroutine_handle<> waiter;
  trantor::EventLoop* loop{nullptr};
};

class PullJoinAwaiter
{
public:
  explicit PullJoinAwaiter(std::shared_ptr<PullJoin> join)
      : join_(std::move(join))
  {
  }

  [[nodiscard]] bool await_ready() const
  {
    std::scoped_lock lock(join_->mutex);
    return join_->pending == 0;
  }

  bool await_suspend(std::coroutine_handle<> handle)
  {
    std::scoped_lock lock(join_->mutex);
    if (join_->pending == 0)
      return false;
    join_->waiter = handle;
    join_->loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    return true;
  }

  void await_resume() const noexcept {}

private:
  std::shared_ptr<PullJoin> join_;
};

void finishPullWorker(const std::shared_ptr<PullJoin>& join)
{
  std::coroutine_handle<> waiter;
  trantor::EventLoop* loop = nullptr;
  {
    std::scoped_lock lock(join->mutex);
    if (--join->pending != 0)
      return;
    waiter = join->waiter;
    loop = join->loop;
  }
  if (!waiter)
    return;
  if (loop)
    loop->queueInLoop([waiter]() { waiter.resume(); });
  else
    waiter.resume();
}

using PullJob = std::function<drogon::Task<void>(std::size_t)>;

drogon::Task<void> runPulls(std::size_t count, PullJob job)
{
  if (count == 0)
    co_return;
  const auto join = std::make_shared<PullJoin>();
  const auto next = std::make_shared<std::atomic<std::size_t>>(0);
  const auto shared = std::make_shared<const PullJob>(std::move(job));
  const std::size_t width = std::min(count, kPullWidth);
  join->pending = width;
  for (std::size_t worker = 0; worker < width; ++worker) {
    drogon::async_run([join, next, shared, count]() -> drogon::Task<> {
      for (std::size_t index = next->fetch_add(1); index < count;
           index = next->fetch_add(1))
        co_await (*shared)(index);
      finishPullWorker(join);
    });
  }
  co_await PullJoinAwaiter(join);
}

struct TablePull
{
  const std::string* name{nullptr};
  TableName table{TableName::User};
  const SynchronizedBodyDto* dto{nullptr};
};

struct PullBatch
{
  std::vector<TablePull> pulls;
  std::vector<Json::Value> nodes;
  std::vector<std::exception_ptr> errors;
  JwtContext ctx;
};

Json::Value emptyTableNode(const SynchronizedBodyDto& dto)
{
  Json::Value node(Json::objectValue);
  node["created"] = Json::arrayValue;
  node["deleted"] = Json::arrayValue;
  if (dto.findLastCreated || dto.findLastDeleted)
    node["lastSyncDate"] = Json::objectValue;
  return node;
}

Json::Value watermarkOf(const Json::Value& lastId, int64_t frontier)
{
  if (lastId.isIntegral() && lastId.asInt64() >= frontier)
    return lastId;
  return {static_cast<Json::Int64>(frontier)};
}
}

const Syncable& SynchronizedService::repoFor(TableName table) const
{
  if (table == TableName::UserActionLog)
    return userActionLogRepository_;
  throw std::invalid_argument("table is not syncable");
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
    SyncFilter filter = applyRange(base, dto.created);
    filter.scopeIds = dto.scope;
    auto rows = co_await repo.find(filter);
    if (!rows.empty()) {
      const auto& last = rows.back();
      node["lastSyncDate"]["createdId"] = last.get("id", Json::Value());
      node["lastSyncDate"]["created"] = last.get("createdAt", Json::Value());
    }
    Json::Value arr(Json::arrayValue);
    for (auto& row : rows)
      arr.append(std::move(row));
    node["created"] = std::move(arr);
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
      darr.append(std::move(record));
    }
    node["deleted"] = std::move(darr);
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
    node["lastSyncDate"] = std::move(last);
  }

  co_return node;
}

drogon::Task<Json::Value> SynchronizedService::syncUserNotification(
    const SynchronizedBodyDto& dto, const JwtContext& ctx) const
{
  if (!notificationSyncSource_)
    throw ResponseException(503, SyncErrors::NotificationSyncUnavailable);

  Json::Value node(Json::objectValue);

  if (dto.requiredCreate) {
    SyncFilter filter;
    if (dto.created) {
      filter.startTime = dto.created->startTime;
      filter.startId = dto.created->startId;
      filter.endTime = dto.created->endTime;
    }
    auto rows = co_await notificationSyncSource_->find(ctx, filter);
    if (!rows.empty()) {
      const auto& last = rows.back();
      node["lastSyncDate"]["createdId"] = last.get("id", Json::Value());
      node["lastSyncDate"]["created"] = last.get("createdAt", Json::Value());
    }
    Json::Value arr(Json::arrayValue);
    for (auto& row : rows)
      arr.append(std::move(row));
    node["created"] = std::move(arr);
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
    node["lastSyncDate"] = std::move(last);
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
  for (const auto table : role_access::moduleTables(role)) {
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
      {"user_action_log", &SynchronizedDto::userActionLog},
  };

  Json::Value out(Json::objectValue);
  const auto batch = std::make_shared<PullBatch>();
  batch->ctx = ctx;
  auto& pulls = batch->pulls;
  pulls.reserve(kBodyFields.size());
  for (const auto& [name, member] : kBodyFields) {
    const auto& field = body.*member;
    if (!field.has_value())
      continue;

    const auto table = tableNameFromString(name);
    if (!role_access::hasAccess(
            {.role = ctx.role, .table = table, .perm = RolePermission::Read})) {
      out[name] = Json::nullValue;
      continue;
    }
    pulls.push_back({.name = &name, .table = table, .dto = &*field});
  }

  batch->nodes.resize(pulls.size());
  batch->errors.resize(pulls.size());
  co_await runPulls(pulls.size(),
                    [this, batch](std::size_t index) -> drogon::Task<void> {
                      const TablePull& pull = batch->pulls[index];
                      try {
                        batch->nodes[index] = co_await pullTable(
                            {.table = pull.table,
                             .dto = *pull.dto,
                             .ctx = batch->ctx});
                      }
                      catch (...) {
                        batch->errors[index] = std::current_exception();
                      }
                    });

  for (std::size_t index = 0; index < pulls.size(); ++index) {
    if (batch->errors[index])
      std::rethrow_exception(batch->errors[index]);
    out[*pulls[index].name] = std::move(batch->nodes[index]);
  }

  SocketEmitDto response;
  response.operation = SyncOperation::Synchronize;
  response.obj = std::move(out);
  co_return response.toJson();
}

drogon::Task<Json::Value>
SynchronizedService::pullTable(const TablePullInput& input) const
{
  const auto table = input.table;
  const auto& dto = input.dto;
  const auto& ctx = input.ctx;

  if (table == TableName::Notification)
    co_return co_await syncUserNotification(dto, ctx);

  if (table == TableName::Event)
    co_return emptyTableNode(dto);

  if (const auto cameraTable = cameraSyncTableFor(table)) {
    if (!cameraSyncSource_ || !cameraSyncSource_->serves(*cameraTable))
      throw ResponseException(503, SyncErrors::CameraSyncUnavailable);
    const auto source = cameraSyncSource_->sourceFor(*cameraTable, ctx);
    co_return co_await syncWithRepo({.repo = *source, .dto = dto}, {});
  }

  if (const auto productivityTable = productivitySyncTableFor(table)) {
    if (!productivitySyncSource_ ||
        !productivitySyncSource_->serves(*productivityTable))
      throw ResponseException(503, SyncErrors::ProductivitySyncUnavailable);
    const auto source =
        productivitySyncSource_->sourceFor(*productivityTable, ctx);
    co_return co_await syncWithRepo({.repo = *source, .dto = dto}, {});
  }

  if (const auto identityTable = identitySyncTableFor(table)) {
    if (!identitySyncSource_ || !identitySyncSource_->serves(*identityTable))
      throw ResponseException(503, SyncErrors::IdentitySyncUnavailable);
    const auto source = identitySyncSource_->sourceFor(*identityTable, ctx);
    co_return co_await syncWithRepo({.repo = *source, .dto = dto}, {});
  }

  const auto& repo = repoFor(table);
  co_return co_await syncWithRepo({.repo = repo, .dto = dto}, {});
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

  std::optional<int64_t> frontier;
  if (body.afterId && *body.afterId > 0) {
    frontier = co_await auditLogRepository_.findCompactionFrontier();
    if (*body.afterId < *frontier)
      throw ResponseException(SyncErrors::ReplicaTooOld);
  }

  Json::Value out(Json::objectValue);
  std::vector<Json::Value> rows;
  if (body.findLast && !body.afterId)
    rows = std::vector<Json::Value>{};
  else
    rows = co_await auditLogRepository_.findSync(filter);
  if (!rows.empty())
    out["nextCursorId"] = rows.back().get("id", Json::Value());
  Json::Value arr(Json::arrayValue);
  for (auto& row : rows)
    arr.append(std::move(row));
  out["info"] = std::move(arr);

  if (body.findLast) {
    const auto last = co_await auditLogRepository_.findLastSync(filter);
    Json::Value record;
    if (last) {
      if (!frontier)
        frontier = co_await auditLogRepository_.findCompactionFrontier();
      record["id"] = (*last).get("id", Json::Value());
      record["lastSyncDate"] = (*last).get("eventTimestamp", Json::Value());
      out["watermarkId"] = watermarkOf(record["id"], *frontier);
    }
    else {
      record = Json::nullValue;
    }
    out["lastSyncRecord"] = std::move(record);
  }

  SocketEmitDto response;
  response.operation = SyncOperation::SynchronizeAuditLog;
  response.obj = std::move(out);
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

  std::optional<int64_t> frontier;
  if (body.afterId && *body.afterId > 0) {
    frontier = co_await userAuditLogRepository_.findCompactionFrontier();
    if (*body.afterId < *frontier)
      throw ResponseException(SyncErrors::ReplicaTooOld);
  }

  Json::Value out(Json::objectValue);
  std::vector<Json::Value> rows;
  if (body.findLast && !body.afterId)
    rows = std::vector<Json::Value>{};
  else
    rows = co_await userAuditLogRepository_.findSync(filter);
  if (!rows.empty())
    out["nextCursorId"] = rows.back().get("id", Json::Value());
  Json::Value arr(Json::arrayValue);
  for (auto& row : rows)
    arr.append(std::move(row));
  out["info"] = std::move(arr);

  if (body.findLast) {
    const auto last = co_await userAuditLogRepository_.findLastSync(filter);
    Json::Value record;
    if (last) {
      if (!frontier)
        frontier = co_await userAuditLogRepository_.findCompactionFrontier();
      record["id"] = (*last).get("id", Json::Value());
      record["lastSyncDate"] = (*last).get("eventTimestamp", Json::Value());
      out["watermarkId"] = watermarkOf(record["id"], *frontier);
    }
    else {
      record = Json::nullValue;
    }
    out["lastSyncRecord"] = std::move(record);
  }

  SocketEmitDto response;
  response.operation = SyncOperation::SynchronizeUserAuditLog;
  response.obj = std::move(out);
  co_return response.toJson();
}
