#include "productivity-sync-rpc-service.hxx"

#include <drogon/drogon.h>
#include <grpc-server-identity.hxx>
#include <shared/contracts/sync-filter.hxx>
#include <trantor/utils/Logger.h>

namespace
{

SyncFilter scopedBase(bool personal, int64_t userId)
{
  SyncFilter base;
  if (personal)
    base.userId = userId;
  return base;
}

void mergeRange(SyncFilter& filter, const argus::productivity::v1::SyncRange& range)
{
  if (range.has_start_time())
    filter.startTime = range.start_time();
  if (range.has_start_id())
    filter.startId = range.start_id();
  if (range.has_end_time())
    filter.endTime = range.end_time();
}

void toProto(const Json::Value& row, argus::productivity::v1::ReminderRow* out)
{
  out->set_id(row["id"].asInt64());
  if (!row["createdBy"].isNull())
    out->set_created_by(row["createdBy"].asInt64());
  out->set_target_user_id(row["targetUserId"].asInt64());
  out->set_title(row["title"].asString());
  out->set_description(row["description"].asString());
  out->set_scheduled_at(row["scheduledAt"].asInt64());
  if (!row["recurrenceRule"].isNull())
    out->set_recurrence_rule(row["recurrenceRule"].asString());
  out->set_is_completed(row["isCompleted"].asBool());
  if (!row["completedAt"].isNull())
    out->set_completed_at(row["completedAt"].asInt64());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row,
             argus::productivity::v1::ReminderDetailRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_reminder_id(row["reminderId"].asInt64());
  if (!row["createdBy"].isNull())
    out->set_created_by(row["createdBy"].asInt64());
  out->set_content(row["content"].asString());
  out->set_status(row["status"].asString());
  out->set_file_paths(row["filePaths"].asString());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row,
             argus::productivity::v1::CalendarEventRow* out)
{
  out->set_id(row["id"].asInt64());
  if (!row["createdBy"].isNull())
    out->set_created_by(row["createdBy"].asInt64());
  out->set_owner_id(row["ownerId"].asInt64());
  if (!row["projectId"].isNull())
    out->set_project_id(row["projectId"].asInt64());
  out->set_title(row["title"].asString());
  out->set_description(row["description"].asString());
  out->set_location(row["location"].asString());
  out->set_color(row["color"].asString());
  out->set_starts_at(row["startsAt"].asInt64());
  if (!row["endsAt"].isNull())
    out->set_ends_at(row["endsAt"].asInt64());
  out->set_is_all_day(row["isAllDay"].asBool());
  if (!row["recurrenceRule"].isNull())
    out->set_recurrence_rule(row["recurrenceRule"].asString());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row,
             argus::productivity::v1::CalendarEventShareRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_calendar_event_id(row["calendarEventId"].asInt64());
  out->set_user_id(row["userId"].asInt64());
  out->set_access(row["access"].asString());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row, argus::productivity::v1::ProjectRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_owner_id(row["ownerId"].asInt64());
  out->set_name(row["name"].asString());
  out->set_description(row["description"].asString());
  out->set_status(row["status"].asString());
  out->set_color(row["color"].asString());
  if (!row["startsAt"].isNull())
    out->set_starts_at(row["startsAt"].asInt64());
  if (!row["targetAt"].isNull())
    out->set_target_at(row["targetAt"].asInt64());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row,
             argus::productivity::v1::ProjectMemberRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_project_id(row["projectId"].asInt64());
  out->set_user_id(row["userId"].asInt64());
  out->set_access(row["access"].asString());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row,
             argus::productivity::v1::ProjectTaskRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_project_id(row["projectId"].asInt64());
  if (!row["createdBy"].isNull())
    out->set_created_by(row["createdBy"].asInt64());
  if (!row["assigneeId"].isNull())
    out->set_assignee_id(row["assigneeId"].asInt64());
  out->set_title(row["title"].asString());
  out->set_status(row["status"].asString());
  out->set_priority(row["priority"].asString());
  if (!row["dueAt"].isNull())
    out->set_due_at(row["dueAt"].asInt64());
  out->set_sort_order(row["sortOrder"].asDouble());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

template <typename TableRows, typename Repo>
struct FillInput
{
  const argus::productivity::v1::TablePull& body;
  TableRows* rows;
  const Repo& repo;
  SyncFilter base;
};

template <typename TableRows, typename Repo>
drogon::Task<void> fill(const FillInput<TableRows, Repo>& input)
{
  const argus::productivity::v1::TablePull& body = input.body;
  TableRows* rows = input.rows;
  const Repo& repo = input.repo;

  if (body.required_create()) {
    SyncFilter filter = input.base;
    if (body.has_created())
      mergeRange(filter, body.created());
    for (const auto& row : co_await repo.find(filter))
      toProto(row, rows->add_created());
  }
  if (body.required_deleted()) {
    SyncFilter filter = input.base;
    if (body.has_deleted())
      mergeRange(filter, body.deleted());
    for (const auto& row : co_await repo.findDeleted(filter)) {
      auto* tombstone = rows->add_deleted();
      tombstone->set_id(row["id"].asInt64());
      tombstone->set_deleted_at(row["deletedAt"].asInt64());
    }
  }
  if (body.find_last_created()) {
    const auto row = co_await repo.findLast(input.base);
    if (row)
      toProto(*row, rows->mutable_last_created());
  }
  if (body.find_last_deleted()) {
    const auto row = co_await repo.findLastDeleted(input.base);
    if (row) {
      rows->mutable_last_deleted()->set_id((*row)["id"].asInt64());
      rows->mutable_last_deleted()->set_deleted_at(
          (*row)["deletedAt"].asInt64());
    }
  }
  co_return;
}

} // namespace

grpc::ServerUnaryReactor* ProductivitySyncRpcService::PullTable(
    grpc::CallbackServerContext* context,
    const argus::productivity::v1::PullTableRequest* request,
    argus::productivity::v1::PullTableResponse* response)
{
  const auto userId = argus::sdk::callerUserId(context);
  if (!userId) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "identity metadata missing or invalid"));
    return reactor;
  }
  if (request->table_case() ==
      argus::productivity::v1::PullTableRequest::TABLE_NOT_SET) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "table branch is required"));
    return reactor;
  }

  const argus::productivity::v1::PullTableRequest pull = *request;
  const int64_t sub = *userId;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, pull, responseWriter,
                                        sub]() {
    drogon::async_run([this, reactor, pull, responseWriter,
                       sub]() -> drogon::Task<void> {
      try {
        switch (pull.table_case()) {
          case argus::productivity::v1::PullTableRequest::kReminder:
            co_await fill(FillInput{.body = pull.reminder(),
                                    .rows = responseWriter->mutable_reminder(),
                                    .repo = reminderRepository_,
                                    .base = scopedBase(false, sub)});
            break;
          case argus::productivity::v1::PullTableRequest::kReminderDetail:
            co_await fill(FillInput{
                .body = pull.reminder_detail(),
                .rows = responseWriter->mutable_reminder_detail(),
                .repo = reminderDetailRepository_,
                .base = scopedBase(false, sub)});
            break;
          case argus::productivity::v1::PullTableRequest::kCalendarEvent:
            co_await fill(FillInput{
                .body = pull.calendar_event(),
                .rows = responseWriter->mutable_calendar_event(),
                .repo = calendarEventRepository_,
                .base = scopedBase(true, sub)});
            break;
          case argus::productivity::v1::PullTableRequest::kCalendarEventShare:
            co_await fill(FillInput{
                .body = pull.calendar_event_share(),
                .rows = responseWriter->mutable_calendar_event_share(),
                .repo = calendarEventShareRepository_,
                .base = scopedBase(true, sub)});
            break;
          case argus::productivity::v1::PullTableRequest::kProject:
            co_await fill(FillInput{.body = pull.project(),
                                    .rows = responseWriter->mutable_project(),
                                    .repo = projectRepository_,
                                    .base = scopedBase(true, sub)});
            break;
          case argus::productivity::v1::PullTableRequest::kProjectMember:
            co_await fill(FillInput{
                .body = pull.project_member(),
                .rows = responseWriter->mutable_project_member(),
                .repo = projectMemberRepository_,
                .base = scopedBase(true, sub)});
            break;
          case argus::productivity::v1::PullTableRequest::kProjectTask:
            co_await fill(FillInput{
                .body = pull.project_task(),
                .rows = responseWriter->mutable_project_task(),
                .repo = projectTaskRepository_,
                .base = scopedBase(true, sub)});
            break;
          default:
            break;
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Productivity sync RPC: PullTable failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}
