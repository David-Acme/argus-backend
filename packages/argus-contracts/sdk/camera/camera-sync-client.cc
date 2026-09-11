#include "camera-sync-client.hxx"

#include <grpc-client-base.hxx>

namespace
{
constexpr int kPullTimeoutMs = 5000;
} // namespace

CameraSyncClient::CameraSyncClient(std::string target)
    : channel_(argus::sdk::makeChannel(target)),
      stub_(argus::camera::v1::SyncService::NewStub(channel_))
{
}

std::optional<argus::camera::v1::PullTableResponse>
CameraSyncClient::pullTable(const argus::camera::v1::PullTableRequest& request,
                            const SyncIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kPullTimeoutMs);
  argus::sdk::addCallerIdentity(context, {.userId = identity.userId,
                                          .role = identity.role,
                                          .device = identity.device});

  argus::camera::v1::PullTableResponse response;
  if (const grpc::Status status = stub_->PullTable(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::camera::v1::ListCatalogResponse>
CameraSyncClient::listCatalog(const SyncIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kPullTimeoutMs);
  argus::sdk::addCallerIdentity(context, {.userId = identity.userId,
                                          .role = identity.role,
                                          .device = identity.device});

  const argus::camera::v1::ListCatalogRequest request;

  argus::camera::v1::ListCatalogResponse response;
  if (const grpc::Status status =
          stub_->ListCatalog(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
