#include "reminder-rpc-service.hxx"

#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <productivity/productivity-errors.hxx>
#include <string>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
using argus::productivity::v1::ReminderRow;

void fill(const ReminderSchema& schema, ReminderRow* out)
{
  out->set_id(schema.id);
  if (schema.createdBy)
    out->set_created_by(*schema.createdBy);
  out->set_target_user_id(schema.targetUserId);
  out->set_title(schema.title);
  out->set_description(schema.description);
  out->set_scheduled_at(schema.scheduledAt);
  if (schema.recurrenceRule)
    out->set_recurrence_rule(*schema.recurrenceRule);
  out->set_is_completed(schema.isCompleted);
  if (schema.completedAt)
    out->set_completed_at(*schema.completedAt);
  out->set_created_at(schema.createdAt);
  if (schema.updatedAt)
    out->set_updated_at(*schema.updatedAt);
  if (schema.deletedAt)
    out->set_deleted_at(*schema.deletedAt);
}

std::string describe(const ValidationException& error)
{
  std::string text = "invalid fields:";
  for (const auto& [field, reasons] : error.errors())
    text += " " + field;
  return text;
}

grpc::Status statusOf(const std::exception& error)
{
  if (const auto* invalid = dynamic_cast<const ValidationException*>(&error))
    return {grpc::StatusCode::INVALID_ARGUMENT, describe(*invalid)};
  if (const auto* refusal = dynamic_cast<const ResponseException*>(&error)) {
    if (refusal->statusCode() == 404)
      return {grpc::StatusCode::NOT_FOUND, refusal->what()};
    if (refusal->statusCode() == 409)
      return {grpc::StatusCode::ALREADY_EXISTS, refusal->what()};
  }
  LOG_WARN << "Reminder RPC failed: " << error.what();
  return {grpc::StatusCode::INTERNAL, "the reminder could not be saved"};
}

grpc::ServerUnaryReactor* refuse(grpc::CallbackServerContext* context,
                                 grpc::Status status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(std::move(status));
  return reactor;
}

struct Caller
{
  int64_t userId{0};
  grpc::ServerUnaryReactor* refused{nullptr};
};

Caller authorize(grpc::CallbackServerContext* context,
                 const std::vector<argus::client::CallerCredential>& callers)
{
  if (!argus::client::authorizeCaller(context, callers).has_value())
    return {.userId = 0,
            .refused = refuse(context,
                              {grpc::StatusCode::UNAUTHENTICATED,
                               "a paired reminder caller credential is required"})};
  const auto userId = argus::client::callerUserId(context);
  if (!userId || *userId <= 0)
    return {.userId = 0,
            .refused = refuse(context,
                              {grpc::StatusCode::UNAUTHENTICATED,
                               "identity metadata missing or invalid"})};
  return {.userId = *userId, .refused = nullptr};
}

template <typename Work>
grpc::ServerUnaryReactor* run(grpc::CallbackServerContext* context, Work work)
{
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop([reactor, work = std::move(work)]() mutable {
    drogon::async_run([reactor, work = std::move(work)]() mutable -> drogon::Task<void> {
      try {
        co_await work();
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        reactor->Finish(statusOf(error));
      }
      co_return;
    });
  });
  return reactor;
}
}

ReminderRpcService::ReminderRpcService()
    : callers_({argus::client::CallerCredential{
          .service = "argus-llm",
          .secret = ConfigService::getString("grpc.caller_llm")}})
{
}

ReminderRpcService::ReminderRpcService(
    std::vector<argus::client::CallerCredential> callers)
    : callers_(std::move(callers))
{
}

grpc::ServerUnaryReactor* ReminderRpcService::CreateReminder(
    grpc::CallbackServerContext* context,
    const argus::productivity::v1::CreateReminderRequest* request,
    argus::productivity::v1::ReminderResponse* response)
{
  const Caller caller = authorize(context, callers_);
  if (caller.refused)
    return caller.refused;
  return run(context, [this, userId = caller.userId, request, response]() -> drogon::Task<void> {
    CreateReminderDto dto;
    dto.title = request->title();
    dto.description = request->description();
    dto.scheduledAt = request->scheduled_at();
    if (request->has_recurrence_rule())
      dto.recurrenceRule = request->recurrence_rule();
    dto = CreateReminderDto::validated(std::move(dto));
    const auto row = co_await service_.create(
        {.body = dto, .userId = userId, .idempotencyKey = request->idempotency_key()});
    fill(row, response->mutable_reminder());
  });
}

grpc::ServerUnaryReactor* ReminderRpcService::UpdateReminder(
    grpc::CallbackServerContext* context,
    const argus::productivity::v1::UpdateReminderRequest* request,
    argus::productivity::v1::ReminderResponse* response)
{
  const Caller caller = authorize(context, callers_);
  if (caller.refused)
    return caller.refused;
  return run(context, [this, userId = caller.userId, request, response]() -> drogon::Task<void> {
    UpdateReminderDto dto;
    if (request->has_title())
      dto.title = request->title();
    if (request->has_description())
      dto.description = request->description();
    if (request->has_scheduled_at())
      dto.scheduledAt = request->scheduled_at();
    if (request->has_is_completed())
      dto.isCompleted = request->is_completed();
    dto = UpdateReminderDto::validated(std::move(dto));
    const auto row = co_await service_.update(
        {.id = request->id(), .body = dto, .userId = userId});
    if (!row)
      throw ResponseException(ProductivityErrors::ReminderNotFound);
    fill(*row, response->mutable_reminder());
  });
}

grpc::ServerUnaryReactor* ReminderRpcService::DeleteReminder(
    grpc::CallbackServerContext* context,
    const argus::productivity::v1::DeleteReminderRequest* request,
    argus::productivity::v1::DeleteReminderResponse*)
{
  const Caller caller = authorize(context, callers_);
  if (caller.refused)
    return caller.refused;
  return run(context, [this, userId = caller.userId, request]() -> drogon::Task<void> {
    if (!co_await service_.remove({.id = request->id(), .userId = userId}))
      throw ResponseException(ProductivityErrors::ReminderNotFound);
  });
}

grpc::ServerUnaryReactor* ReminderRpcService::GetReminder(
    grpc::CallbackServerContext* context,
    const argus::productivity::v1::GetReminderRequest* request,
    argus::productivity::v1::ReminderResponse* response)
{
  const Caller caller = authorize(context, callers_);
  if (caller.refused)
    return caller.refused;
  return run(context, [this, userId = caller.userId, request, response]() -> drogon::Task<void> {
    const auto row = co_await service_.get({.id = request->id(), .userId = userId});
    if (!row)
      throw ResponseException(ProductivityErrors::ReminderNotFound);
    fill(*row, response->mutable_reminder());
  });
}

grpc::ServerUnaryReactor* ReminderRpcService::ListReminders(
    grpc::CallbackServerContext* context,
    const argus::productivity::v1::ListRemindersRequest* request,
    argus::productivity::v1::ListRemindersResponse* response)
{
  const Caller caller = authorize(context, callers_);
  if (caller.refused)
    return caller.refused;
  return run(context, [this, userId = caller.userId, request, response]() -> drogon::Task<void> {
    const auto rows = co_await service_.list(
        {.userId = userId,
         .includeCompleted = request->include_completed(),
         .limit = request->limit() > 0 ? request->limit()
                                       : reminder_query::kListLimit});
    for (const auto& row : rows)
      fill(row, response->add_reminders());
  });
}
