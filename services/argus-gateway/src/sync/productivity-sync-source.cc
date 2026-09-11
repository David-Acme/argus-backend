#include "productivity-sync-source.hxx"

#include <config/app-config.hxx>
#include <json/value.h>
#include <shared/contracts/sync-filter.hxx>
#include <shared/enums.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <string>
#include <utility>
#include <vector>

namespace
{
using argus::productivity::v1::PullTableRequest;
using argus::productivity::v1::PullTableResponse;
using argus::productivity::v1::TablePull;

argus::sdk::CallerIdentity identityFor(const JwtContext& ctx)
{
  return {.userId = ctx.sub,
          .role = userRoleToString(ctx.role),
          .device = ctx.deviceHash};
}

argus::productivity::v1::SyncRange toRange(const SyncFilter& filter)
{
  argus::productivity::v1::SyncRange range;
  if (filter.startTime)
    range.set_start_time(*filter.startTime);
  if (filter.startId)
    range.set_start_id(*filter.startId);
  if (filter.endTime)
    range.set_end_time(*filter.endTime);
  return range;
}

TablePull* mutablePull(PullTableRequest& request, ProductivitySyncTable table)
{
  switch (table) {
    case ProductivitySyncTable::Reminder:
      return request.mutable_reminder();
    case ProductivitySyncTable::ReminderDetail:
      return request.mutable_reminder_detail();
    case ProductivitySyncTable::CalendarEvent:
      return request.mutable_calendar_event();
    case ProductivitySyncTable::CalendarEventShare:
      return request.mutable_calendar_event_share();
    case ProductivitySyncTable::Project:
      return request.mutable_project();
    case ProductivitySyncTable::ProjectMember:
      return request.mutable_project_member();
    case ProductivitySyncTable::ProjectTask:
      return request.mutable_project_task();
  }
  return nullptr;
}

Json::Value reminderRowToJson(const argus::productivity::v1::ReminderRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["createdBy"] = row.has_created_by()
                          ? Json::Value(Json::Int64(row.created_by()))
                          : Json::Value();
  json["targetUserId"] = Json::Int64(row.target_user_id());
  json["title"] = row.title();
  json["description"] = row.description();
  json["scheduledAt"] = Json::Int64(row.scheduled_at());
  json["recurrenceRule"] = row.has_recurrence_rule()
                               ? Json::Value(row.recurrence_rule())
                               : Json::Value();
  json["isCompleted"] = row.is_completed();
  json["completedAt"] = row.has_completed_at()
                            ? Json::Value(Json::Int64(row.completed_at()))
                            : Json::Value();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value
reminderDetailRowToJson(const argus::productivity::v1::ReminderDetailRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["reminderId"] = Json::Int64(row.reminder_id());
  json["createdBy"] = row.has_created_by()
                          ? Json::Value(Json::Int64(row.created_by()))
                          : Json::Value();
  json["content"] = row.content();
  json["status"] = row.status();
  json["filePaths"] = row.file_paths();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value
calendarEventRowToJson(const argus::productivity::v1::CalendarEventRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["createdBy"] = row.has_created_by()
                          ? Json::Value(Json::Int64(row.created_by()))
                          : Json::Value();
  json["ownerId"] = Json::Int64(row.owner_id());
  json["projectId"] = row.has_project_id()
                          ? Json::Value(Json::Int64(row.project_id()))
                          : Json::Value();
  json["title"] = row.title();
  json["description"] = row.description();
  json["location"] = row.location();
  json["color"] = row.color();
  json["startsAt"] = Json::Int64(row.starts_at());
  json["endsAt"] = row.has_ends_at()
                       ? Json::Value(Json::Int64(row.ends_at()))
                       : Json::Value();
  json["isAllDay"] = row.is_all_day();
  json["recurrenceRule"] = row.has_recurrence_rule()
                               ? Json::Value(row.recurrence_rule())
                               : Json::Value();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value calendarEventShareRowToJson(
    const argus::productivity::v1::CalendarEventShareRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["calendarEventId"] = Json::Int64(row.calendar_event_id());
  json["userId"] = Json::Int64(row.user_id());
  json["access"] = row.access();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value projectRowToJson(const argus::productivity::v1::ProjectRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["ownerId"] = Json::Int64(row.owner_id());
  json["name"] = row.name();
  json["description"] = row.description();
  json["status"] = row.status();
  json["color"] = row.color();
  json["startsAt"] = row.has_starts_at()
                         ? Json::Value(Json::Int64(row.starts_at()))
                         : Json::Value();
  json["targetAt"] = row.has_target_at()
                         ? Json::Value(Json::Int64(row.target_at()))
                         : Json::Value();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value
projectMemberRowToJson(const argus::productivity::v1::ProjectMemberRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["projectId"] = Json::Int64(row.project_id());
  json["userId"] = Json::Int64(row.user_id());
  json["access"] = row.access();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value projectTaskRowToJson(const argus::productivity::v1::ProjectTaskRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["projectId"] = Json::Int64(row.project_id());
  json["createdBy"] = row.has_created_by()
                          ? Json::Value(Json::Int64(row.created_by()))
                          : Json::Value();
  json["assigneeId"] = row.has_assignee_id()
                           ? Json::Value(Json::Int64(row.assignee_id()))
                           : Json::Value();
  json["title"] = row.title();
  json["status"] = row.status();
  json["priority"] = row.priority();
  json["dueAt"] = row.has_due_at()
                      ? Json::Value(Json::Int64(row.due_at()))
                      : Json::Value();
  json["sortOrder"] = row.sort_order();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value deletedRowToJson(const argus::productivity::v1::DeletedRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["deletedAt"] = Json::Int64(row.deleted_at());
  return json;
}

template <typename RowRange, typename Convert>
std::vector<Json::Value> convertRows(const RowRange& rows, Convert convert)
{
  std::vector<Json::Value> out;
  out.reserve(rows.size());
  for (const auto& row : rows)
    out.push_back(convert(row));
  return out;
}

template <typename Rows, typename Convert>
std::optional<Json::Value> lastCreatedOf(const Rows& rows, Convert convert)
{
  if (!rows.has_last_created())
    return std::nullopt;
  return convert(rows.last_created());
}

template <typename Rows>
std::optional<Json::Value> lastDeletedOf(const Rows& rows)
{
  if (!rows.has_last_deleted())
    return std::nullopt;
  return deletedRowToJson(rows.last_deleted());
}

ResponseException unavailable()
{
  return ResponseException({.message = "Productivity sync unavailable",
                            .statusCode = 503,
                            .errorCode =
                                AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});
}

std::vector<Json::Value> createdRows(const PullTableResponse& response)
{
  switch (response.table_case()) {
    case PullTableResponse::kReminder:
      return convertRows(response.reminder().created(), reminderRowToJson);
    case PullTableResponse::kReminderDetail:
      return convertRows(response.reminder_detail().created(),
                         reminderDetailRowToJson);
    case PullTableResponse::kCalendarEvent:
      return convertRows(response.calendar_event().created(),
                         calendarEventRowToJson);
    case PullTableResponse::kCalendarEventShare:
      return convertRows(response.calendar_event_share().created(),
                         calendarEventShareRowToJson);
    case PullTableResponse::kProject:
      return convertRows(response.project().created(), projectRowToJson);
    case PullTableResponse::kProjectMember:
      return convertRows(response.project_member().created(),
                         projectMemberRowToJson);
    case PullTableResponse::kProjectTask:
      return convertRows(response.project_task().created(), projectTaskRowToJson);
    default:
      throw unavailable();
  }
}

std::vector<Json::Value> deletedRows(const PullTableResponse& response)
{
  switch (response.table_case()) {
    case PullTableResponse::kReminder:
      return convertRows(response.reminder().deleted(), deletedRowToJson);
    case PullTableResponse::kReminderDetail:
      return convertRows(response.reminder_detail().deleted(), deletedRowToJson);
    case PullTableResponse::kCalendarEvent:
      return convertRows(response.calendar_event().deleted(), deletedRowToJson);
    case PullTableResponse::kCalendarEventShare:
      return convertRows(response.calendar_event_share().deleted(),
                         deletedRowToJson);
    case PullTableResponse::kProject:
      return convertRows(response.project().deleted(), deletedRowToJson);
    case PullTableResponse::kProjectMember:
      return convertRows(response.project_member().deleted(), deletedRowToJson);
    case PullTableResponse::kProjectTask:
      return convertRows(response.project_task().deleted(), deletedRowToJson);
    default:
      throw unavailable();
  }
}

std::optional<Json::Value> lastCreated(const PullTableResponse& response)
{
  switch (response.table_case()) {
    case PullTableResponse::kReminder:
      return lastCreatedOf(response.reminder(), reminderRowToJson);
    case PullTableResponse::kReminderDetail:
      return lastCreatedOf(response.reminder_detail(), reminderDetailRowToJson);
    case PullTableResponse::kCalendarEvent:
      return lastCreatedOf(response.calendar_event(), calendarEventRowToJson);
    case PullTableResponse::kCalendarEventShare:
      return lastCreatedOf(response.calendar_event_share(),
                           calendarEventShareRowToJson);
    case PullTableResponse::kProject:
      return lastCreatedOf(response.project(), projectRowToJson);
    case PullTableResponse::kProjectMember:
      return lastCreatedOf(response.project_member(), projectMemberRowToJson);
    case PullTableResponse::kProjectTask:
      return lastCreatedOf(response.project_task(), projectTaskRowToJson);
    default:
      return std::nullopt;
  }
}

std::optional<Json::Value> lastDeleted(const PullTableResponse& response)
{
  switch (response.table_case()) {
    case PullTableResponse::kReminder:
      return lastDeletedOf(response.reminder());
    case PullTableResponse::kReminderDetail:
      return lastDeletedOf(response.reminder_detail());
    case PullTableResponse::kCalendarEvent:
      return lastDeletedOf(response.calendar_event());
    case PullTableResponse::kCalendarEventShare:
      return lastDeletedOf(response.calendar_event_share());
    case PullTableResponse::kProject:
      return lastDeletedOf(response.project());
    case PullTableResponse::kProjectMember:
      return lastDeletedOf(response.project_member());
    case PullTableResponse::kProjectTask:
      return lastDeletedOf(response.project_task());
    default:
      return std::nullopt;
  }
}

} // namespace

class ProductivitySyncGateway::Pull : public Syncable
{
public:
  struct Deps
  {
    std::shared_ptr<ProductivitySyncClient> client;
    ProductivitySyncTable table{ProductivitySyncTable::Reminder};
    argus::sdk::CallerIdentity identity;
  };

  explicit Pull(const Deps& deps)
      : client_(deps.client), table_(deps.table), identity_(deps.identity)
  {
  }

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override
  {
    const auto response = co_await pull(
        {.table = table_, .mode = Mode::Created, .filter = filter});
    co_return createdRows(response);
  }

  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override
  {
    const auto response = co_await pull(
        {.table = table_, .mode = Mode::Deleted, .filter = filter});
    co_return deletedRows(response);
  }

  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter&) const override
  {
    const auto response =
        co_await pull({.table = table_, .mode = Mode::LastCreated, .filter = {}});
    co_return lastCreated(response);
  }

  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter&) const override
  {
    const auto response =
        co_await pull({.table = table_, .mode = Mode::LastDeleted, .filter = {}});
    co_return lastDeleted(response);
  }

private:
  enum class Mode
  {
    Created,
    Deleted,
    LastCreated,
    LastDeleted,
  };

  struct RequestInput
  {
    ProductivitySyncTable table{ProductivitySyncTable::Reminder};
    Mode mode{Mode::Created};
    const SyncFilter& filter;
  };

  static PullTableRequest requestFor(const RequestInput& input)
  {
    PullTableRequest request;
    auto* body = mutablePull(request, input.table);
    switch (input.mode) {
      case Mode::Created:
        body->set_required_create(true);
        *body->mutable_created() = toRange(input.filter);
        break;
      case Mode::Deleted:
        body->set_required_deleted(true);
        *body->mutable_deleted() = toRange(input.filter);
        break;
      case Mode::LastCreated:
        body->set_find_last_created(true);
        break;
      case Mode::LastDeleted:
        body->set_find_last_deleted(true);
        break;
    }
    return request;
  }

  struct PullInput
  {
    ProductivitySyncTable table{ProductivitySyncTable::Reminder};
    Mode mode{Mode::Created};
    const SyncFilter& filter;
  };

  drogon::Task<PullTableResponse> pull(const PullInput& input) const
  {
    const auto request = requestFor(
        {.table = input.table, .mode = input.mode, .filter = input.filter});
    auto response = co_await BlockingTask<std::optional<PullTableResponse>>(
        [this, request]() { return client_->pullTable(request, identity_); });
    if (!response)
      throw unavailable();
    co_return std::move(*response);
  }

  std::shared_ptr<ProductivitySyncClient> client_;
  ProductivitySyncTable table_;
  argus::sdk::CallerIdentity identity_;
};

ProductivitySyncGateway::ProductivitySyncGateway(std::string target)
    : client_(std::make_shared<ProductivitySyncClient>(std::move(target)))
{
}

bool ProductivitySyncGateway::serves(ProductivitySyncTable) const
{
  return true;
}

std::unique_ptr<Syncable> ProductivitySyncGateway::sourceFor(
    ProductivitySyncTable table, const JwtContext& ctx) const
{
  return std::make_unique<Pull>(
      Pull::Deps{.client = client_, .table = table, .identity = identityFor(ctx)});
}
