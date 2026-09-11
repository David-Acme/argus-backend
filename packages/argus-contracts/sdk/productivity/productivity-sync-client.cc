#include "productivity-sync-client.hxx"

#include <grpc-client-base.hxx>

namespace
{
constexpr int kPullTimeoutMs = 5000;
} // namespace

ProductivitySyncClient::ProductivitySyncClient(std::string target)
    : channel_(argus::sdk::makeChannel(target)),
      stub_(argus::productivity::v1::SyncService::NewStub(channel_))
{
}

std::optional<argus::productivity::v1::PullTableResponse>
ProductivitySyncClient::pullTable(
    const argus::productivity::v1::PullTableRequest& request,
    const argus::sdk::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kPullTimeoutMs);
  argus::sdk::addCallerIdentity(context, identity);

  argus::productivity::v1::PullTableResponse response;
  if (const grpc::Status status = stub_->PullTable(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
