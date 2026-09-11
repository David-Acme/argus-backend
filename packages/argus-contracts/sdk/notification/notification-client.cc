#include "notification-client.hxx"

#include <grpc-client-base.hxx>

namespace
{
constexpr int kCallTimeoutMs = 5000;
} // namespace

NotificationClient::NotificationClient(std::string target)
    : channel_(argus::sdk::makeChannel(target)),
      stub_(argus::notification::v1::NotificationService::NewStub(channel_))
{
}

std::optional<argus::notification::v1::CreateNotificationsResponse>
NotificationClient::createNotifications(
    const argus::notification::v1::CreateNotificationsRequest& request,
    const argus::sdk::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addCallerIdentity(context, identity);

  argus::notification::v1::CreateNotificationsResponse response;
  if (const grpc::Status status =
          stub_->CreateNotifications(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::notification::v1::PullNotificationsResponse>
NotificationClient::pullNotifications(
    const argus::notification::v1::PullNotificationsRequest& request,
    const argus::sdk::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addCallerIdentity(context, identity);

  argus::notification::v1::PullNotificationsResponse response;
  if (const grpc::Status status =
          stub_->PullNotifications(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
