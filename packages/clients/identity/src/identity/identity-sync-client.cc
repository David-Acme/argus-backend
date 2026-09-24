#include "identity-sync-client.hxx"

namespace
{
constexpr int kPullTimeoutMs = 5000;
}

IdentitySyncClient::IdentitySyncClient(IdentitySyncClientConfig config)
    : fleetSecret_(std::move(config.fleetSecret)),
      channel_(argus::client::makeChannel(config.target)),
      stub_(argus::identity::v1::SyncService::NewStub(channel_))
{
}

std::optional<argus::identity::v1::PullTableResponse>
IdentitySyncClient::pullTable(
    const argus::identity::v1::PullTableRequest& request,
    const argus::client::CallerIdentity& identity) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kPullTimeoutMs);
  argus::client::addFleetSecret(context, fleetSecret_);
  argus::client::addCallerIdentity(context, identity);

  argus::identity::v1::PullTableResponse response;
  if (const grpc::Status status =
          stub_->PullTable(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
