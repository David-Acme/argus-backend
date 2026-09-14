#include "notification-client.hxx"

#include <grpc-client-base.hxx>
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
} // namespace

NotificationClient::NotificationClient(NotificationClientConfig config)
    : channel_(argus::sdk::makeChannel(config.target)),
      stub_(argus::notification::v1::NotificationService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

NotificationCreateResult NotificationClient::createNotifications(
    const argus::notification::v1::CreateNotificationsRequest& request,
    const argus::sdk::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addCallerIdentity(context, identity);
  argus::sdk::addCallerCredential(context, credential_);

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
    const argus::sdk::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addCallerIdentity(context, identity);
  argus::sdk::addCallerCredential(context, credential_);

  NotificationPullResult result;
  const grpc::Status status =
      stub_->PullNotifications(&context, request, &result.response);
  result.status = status;
  result.outcome = status.ok() ? NotificationRpcOutcome::Success
                               : outcomeForStatus(status.error_code());
  return result;
}
