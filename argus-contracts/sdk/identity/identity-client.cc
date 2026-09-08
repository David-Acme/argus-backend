#include "identity-client.hxx"

#include <chrono>

namespace
{
constexpr int kUpdateTimeoutMs = 5000;
} // namespace

IdentityClient::IdentityClient(std::string target)
    : channel_(grpc::CreateChannel(target, grpc::InsecureChannelCredentials())),
      stub_(argus::identity::v1::IdentityService::NewStub(channel_))
{
}

std::optional<argus::identity::v1::UserIdentity>
IdentityClient::updateUserName(const UpdateUserNameInput& input) const
{
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(kUpdateTimeoutMs));
  context.AddMetadata("x-argus-user", std::to_string(input.userId));
  context.AddMetadata("x-argus-role", input.role);

  argus::identity::v1::UpdateUserRequest request;
  request.set_user_id(input.userId);
  request.set_name(input.name);

  argus::identity::v1::UpdateUserResponse response;
  if (const grpc::Status status = stub_->UpdateUser(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response.user();
}
