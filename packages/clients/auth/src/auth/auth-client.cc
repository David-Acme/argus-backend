#include "auth-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <utility>

namespace
{
constexpr int kCallTimeoutMs = 5000;
}

AuthClient::AuthClient(AuthClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::auth::v1::AuthService::NewStub(channel_)),
      credential_({.credential = std::move(config.credential),
                   .fleetSecret = std::move(config.fleetSecret)})
{
}

std::optional<argus::auth::v1::ValidateTokenResponse>
AuthClient::validateToken(const ValidateSessionInput& input) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addPeerCredential(context, credential_);

  argus::auth::v1::ValidateTokenRequest request;
  request.set_access_token(input.accessToken);
  if (input.hasDeviceContext)
    request.set_device_hash(input.deviceHash);
  if (!input.origin.empty())
    request.set_origin(input.origin);

  argus::auth::v1::ValidateTokenResponse response;
  if (const grpc::Status status =
          stub_->ValidateToken(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<bool>
AuthClient::checkDeviceCredential(const std::string& secretHash) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addPeerCredential(context, credential_);

  argus::auth::v1::CheckDeviceCredentialRequest request;
  request.set_secret_hash(secretHash);

  argus::auth::v1::CheckDeviceCredentialResponse response;
  if (const grpc::Status status =
          stub_->CheckDeviceCredential(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response.active();
}
