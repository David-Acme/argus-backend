#include "camera-sync-client.hxx"

#include <chrono>

namespace
{
constexpr int kPullTimeoutMs = 5000;
} // namespace

CameraSyncClient::CameraSyncClient(std::string target)
    : channel_(grpc::CreateChannel(target, grpc::InsecureChannelCredentials())),
      stub_(argus::camera::v1::SyncService::NewStub(channel_))
{
}

std::optional<argus::camera::v1::PullTableResponse>
CameraSyncClient::pullTable(const argus::camera::v1::PullTableRequest& request,
                            const SyncIdentity& identity) const
{
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(kPullTimeoutMs));
  context.AddMetadata("x-argus-user", std::to_string(identity.userId));
  context.AddMetadata("x-argus-role", identity.role);
  context.AddMetadata("x-argus-device", identity.device);

  argus::camera::v1::PullTableResponse response;
  if (const grpc::Status status = stub_->PullTable(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
