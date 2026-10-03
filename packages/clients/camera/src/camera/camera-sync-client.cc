#include "camera-sync-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <utility>

namespace
{
constexpr int kPullTimeoutMs = 5000;
}

CameraSyncClient::CameraSyncClient(CameraSyncClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::camera::v1::SyncService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

std::optional<argus::camera::v1::PullTableResponse>
CameraSyncClient::pullTable(const argus::camera::v1::PullTableRequest& request,
                            const SyncIdentity& identity) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kPullTimeoutMs);
  argus::client::addCallerIdentity(context, {.userId = identity.userId,
                                          .role = identity.role,
                                          .device = identity.device});
  argus::client::addCallerCredential(context, credential_);

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
  argus::client::setDeadline(context, kPullTimeoutMs);
  argus::client::addCallerIdentity(context, {.userId = identity.userId,
                                          .role = identity.role,
                                          .device = identity.device});
  argus::client::addCallerCredential(context, credential_);

  const argus::camera::v1::ListCatalogRequest request;

  argus::camera::v1::ListCatalogResponse response;
  if (const grpc::Status status =
          stub_->ListCatalog(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
