#include "identity-client.hxx"

#include <grpc-client-base.hxx>

namespace
{
constexpr int kCallTimeoutMs = 5000;
} // namespace

IdentityClient::IdentityClient(std::string target, std::string fleetSecret)
    : channel_(argus::sdk::makeChannel(target)),
      stub_(argus::identity::v1::IdentityService::NewStub(channel_)),
      fleetSecret_(std::move(fleetSecret))
{
}

std::optional<argus::identity::v1::UserIdentity>
IdentityClient::updateUserName(const UpdateUserNameInput& input) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);
  argus::sdk::addCallerIdentity(context,
                                {.userId = input.userId, .role = input.role});

  argus::identity::v1::UpdateUserRequest request;
  request.set_user_id(input.userId);
  request.set_name(input.name);

  argus::identity::v1::UpdateUserResponse response;
  if (const grpc::Status status = stub_->UpdateUser(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response.user();
}

std::optional<argus::identity::v1::ValidateTokenResponse>
IdentityClient::validateToken(const ValidateTokenInput& input) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::ValidateTokenRequest request;
  request.set_access_token(input.accessToken);
  if (input.hasDeviceContext)
    request.set_device_hash(input.deviceHash);

  argus::identity::v1::ValidateTokenResponse response;
  if (const grpc::Status status =
          stub_->ValidateToken(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

bool IdentityClient::checkDeviceCredential(const std::string& secretHash) const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::CheckDeviceCredentialRequest request;
  request.set_secret_hash(secretHash);

  argus::identity::v1::CheckDeviceCredentialResponse response;
  if (const grpc::Status status =
          stub_->CheckDeviceCredential(&context, request, &response);
      !status.ok())
    return false;
  return response.active();
}
