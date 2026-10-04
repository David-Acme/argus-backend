#include "notification-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <utility>

namespace
{
constexpr int kCallTimeoutMs = 5000;

NotificationRpcOutcome outcomeForStatus(grpc::StatusCode code)
{
  switch (code) {
    case grpc::StatusCode::ALREADY_EXISTS:
      return NotificationRpcOutcome::Conflict;
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return NotificationRpcOutcome::Unavailable;
    default:
      return NotificationRpcOutcome::Rejected;
  }
}
}

NotificationClient::NotificationClient(NotificationClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      credential_(std::move(config.credential)),
      stub_(argus::notification::v1::NotificationService::NewStub(channel_)),
      callStub_(argus::notification::v1::CallService::NewStub(channel_))
{
}

NotificationCreateResult NotificationClient::createNotifications(
    const argus::notification::v1::CreateNotificationsRequest& request,
    const argus::client::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerIdentity(context, identity);
  argus::client::addCallerCredential(context, credential_);

  argus::notification::v1::CreateNotificationsResponse response;
  const grpc::Status status =
      stub_->CreateNotifications(&context, request, &response);
  NotificationCreateResult result;
  result.status = status;
  result.created = response.created();
  result.duplicate = response.duplicate();
  result.outcome = status.ok() ? NotificationRpcOutcome::Success
                               : outcomeForStatus(status.error_code());
  return result;
}

NotificationPullResult NotificationClient::pullNotifications(
    const argus::notification::v1::PullNotificationsRequest& request,
    const argus::client::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerIdentity(context, identity);
  argus::client::addCallerCredential(context, credential_);

  NotificationPullResult result;
  const grpc::Status status =
      stub_->PullNotifications(&context, request, &result.response);
  result.status = status;
  result.outcome = status.ok() ? NotificationRpcOutcome::Success
                               : outcomeForStatus(status.error_code());
  return result;
}

NotificationCallClaimResult
NotificationClient::claimCall(const NotificationCallClaimInput& input) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::notification::v1::ClaimCallRequest request;
  request.set_call_id(input.callId);
  request.set_user_id(input.userId);
  request.set_session_id(input.sessionId);

  NotificationCallClaimResult result;
  result.status = callStub_->ClaimCall(&context, request, &result.response);
  result.outcome = result.status.ok()
                       ? NotificationRpcOutcome::Success
                       : outcomeForStatus(result.status.error_code());
  return result;
}

NotificationRpcOutcome
NotificationClient::endCall(const NotificationCallEndInput& input) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::notification::v1::EndCallRequest request;
  request.set_call_id(input.callId);
  request.set_user_id(input.userId);
  request.set_outcome(input.outcome);
  request.set_spoken(input.spoken);

  argus::notification::v1::EndCallResponse response;
  const grpc::Status status = callStub_->EndCall(&context, request, &response);
  if (!status.ok())
    return outcomeForStatus(status.error_code());
  return response.ok() ? NotificationRpcOutcome::Success
                       : NotificationRpcOutcome::Rejected;
}

NotificationCallScheduleResult NotificationClient::scheduleCall(
    const NotificationCallScheduleInput& input) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::notification::v1::ScheduleCallRequest request;
  request.set_user_id(input.userId);
  request.set_fire_at(input.fireAt);
  request.set_topic(input.topic);
  request.set_lang(input.lang);
  request.set_command_id(input.commandId);

  argus::notification::v1::ScheduleCallResponse response;
  NotificationCallScheduleResult result;
  result.status = callStub_->ScheduleCall(&context, request, &response);
  result.outcome = result.status.ok()
                       ? NotificationRpcOutcome::Success
                       : outcomeForStatus(result.status.error_code());
  result.scheduledId = response.scheduled_id();
  result.duplicate = response.duplicate();
  return result;
}
