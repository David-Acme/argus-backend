#include "productivity-reminder-client.hxx"

#include <grpc/grpc-client-base.hxx>

#include <utility>

namespace
{
constexpr int kCallTimeoutMs = 5000;

ReminderRpcOutcome outcomeOf(const grpc::Status& status)
{
  if (status.ok())
    return ReminderRpcOutcome::Success;
  switch (status.error_code()) {
    case grpc::StatusCode::NOT_FOUND:
      return ReminderRpcOutcome::NotFound;
    case grpc::StatusCode::INVALID_ARGUMENT:
      return ReminderRpcOutcome::Invalid;
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return ReminderRpcOutcome::Unavailable;
    default:
      return ReminderRpcOutcome::Refused;
  }
}

void prepare(grpc::ClientContext& context,
             const argus::client::CallerIdentity& identity,
             const std::string& credential)
{
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerIdentity(context, identity);
  argus::client::addCallerCredential(context, credential);
}

ReminderResult resultOf(const grpc::Status& status,
                        const argus::productivity::v1::ReminderResponse& response)
{
  ReminderResult result;
  result.outcome = outcomeOf(status);
  result.message = status.error_message();
  if (status.ok() && response.has_reminder())
    result.reminder = response.reminder();
  return result;
}
}

ProductivityReminderClient::ProductivityReminderClient(ReminderClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::productivity::v1::ReminderService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

ReminderResult
ProductivityReminderClient::create(const ReminderCreateCall& call) const
{
  grpc::ClientContext context;
  prepare(context, call.identity, credential_);
  argus::productivity::v1::CreateReminderRequest request;
  request.set_title(call.title);
  request.set_description(call.description);
  request.set_scheduled_at(call.scheduledAt);
  if (call.recurrenceRule)
    request.set_recurrence_rule(*call.recurrenceRule);
  request.set_idempotency_key(call.idempotencyKey);
  argus::productivity::v1::ReminderResponse response;
  const grpc::Status status = stub_->CreateReminder(&context, request, &response);
  return resultOf(status, response);
}

ReminderResult
ProductivityReminderClient::update(const ReminderUpdateCall& call) const
{
  grpc::ClientContext context;
  prepare(context, call.identity, credential_);
  argus::productivity::v1::UpdateReminderRequest request;
  request.set_id(call.id);
  if (call.title)
    request.set_title(*call.title);
  if (call.description)
    request.set_description(*call.description);
  if (call.scheduledAt)
    request.set_scheduled_at(*call.scheduledAt);
  if (call.isCompleted)
    request.set_is_completed(*call.isCompleted);
  argus::productivity::v1::ReminderResponse response;
  const grpc::Status status = stub_->UpdateReminder(&context, request, &response);
  return resultOf(status, response);
}

ReminderRpcOutcome
ProductivityReminderClient::remove(const ReminderRefCall& call) const
{
  grpc::ClientContext context;
  prepare(context, call.identity, credential_);
  argus::productivity::v1::DeleteReminderRequest request;
  request.set_id(call.id);
  argus::productivity::v1::DeleteReminderResponse response;
  return outcomeOf(stub_->DeleteReminder(&context, request, &response));
}

ReminderResult ProductivityReminderClient::get(const ReminderRefCall& call) const
{
  grpc::ClientContext context;
  prepare(context, call.identity, credential_);
  argus::productivity::v1::GetReminderRequest request;
  request.set_id(call.id);
  argus::productivity::v1::ReminderResponse response;
  const grpc::Status status = stub_->GetReminder(&context, request, &response);
  return resultOf(status, response);
}

ReminderListResult
ProductivityReminderClient::list(const ReminderListCall& call) const
{
  grpc::ClientContext context;
  prepare(context, call.identity, credential_);
  argus::productivity::v1::ListRemindersRequest request;
  request.set_include_completed(call.includeCompleted);
  request.set_limit(call.limit);
  argus::productivity::v1::ListRemindersResponse response;
  const grpc::Status status = stub_->ListReminders(&context, request, &response);
  ReminderListResult result;
  result.outcome = outcomeOf(status);
  result.message = status.error_message();
  if (status.ok())
    result.reminders.assign(response.reminders().begin(), response.reminders().end());
  return result;
}
