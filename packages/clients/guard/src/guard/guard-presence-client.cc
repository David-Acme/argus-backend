#include "guard-presence-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <utility>

GuardPresenceClient::GuardPresenceClient(GuardPresenceClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::guard::v1::PresenceService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

std::optional<argus::guard::v1::ListPresenceResponse>
GuardPresenceClient::listPresence(const std::vector<int64_t>& userIds) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::guard::v1::ListPresenceRequest request;
  for (const int64_t userId : userIds)
    request.add_user_ids(userId);

  argus::guard::v1::ListPresenceResponse response;
  if (const grpc::Status status =
          stub_->ListPresence(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<bool> GuardPresenceClient::onDuty(int64_t userId) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::guard::v1::DutyRequest request;
  request.set_user_id(userId);

  argus::guard::v1::DutyResponse response;
  if (const grpc::Status status = stub_->GetDuty(&context, request, &response); !status.ok())
    return std::nullopt;
  return response.on_duty();
}

std::optional<std::vector<argus::guard::v1::CameraRoom>> GuardPresenceClient::cameraRooms() const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  const argus::guard::v1::CameraRoomsRequest request;
  argus::guard::v1::CameraRoomsResponse response;
  if (const grpc::Status status = stub_->CameraRooms(&context, request, &response); !status.ok())
    return std::nullopt;
  return std::vector<argus::guard::v1::CameraRoom>(response.cameras().begin(), response.cameras().end());
}
