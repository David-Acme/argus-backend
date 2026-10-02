#include "productivity-sync-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <utility>

namespace
{
constexpr int kPullTimeoutMs = 5000;
}

ProductivitySyncClient::ProductivitySyncClient(
    ProductivitySyncClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::productivity::v1::SyncService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

std::optional<argus::productivity::v1::PullTableResponse>
ProductivitySyncClient::pullTable(
    const argus::productivity::v1::PullTableRequest& request,
    const argus::client::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kPullTimeoutMs);
  argus::client::addCallerIdentity(context, identity);
  argus::client::addCallerCredential(context, credential_);

  argus::productivity::v1::PullTableResponse response;
  if (const grpc::Status status = stub_->PullTable(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
