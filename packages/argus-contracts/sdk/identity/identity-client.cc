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

std::optional<argus::identity::v1::GetUserResponse>
IdentityClient::getUser(int64_t userId) const
{
  if (userId <= 0)
    return std::nullopt;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::GetUserRequest request;
  request.set_user_id(userId);

  argus::identity::v1::GetUserResponse response;
  if (const grpc::Status status = stub_->GetUser(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::ListPersonsResponse>
IdentityClient::listPersons() const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  const argus::identity::v1::ListPersonsRequest request;

  argus::identity::v1::ListPersonsResponse response;
  if (const grpc::Status status =
          stub_->ListPersons(&context, request, &response);
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

std::optional<argus::identity::v1::IdentifyPersonResponse>
IdentityClient::identifyPerson(const std::string& image) const
{
  if (image.empty())
    return std::nullopt;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::IdentifyPersonRequest request;
  request.set_image(image);

  argus::identity::v1::IdentifyPersonResponse response;
  if (const grpc::Status status =
          stub_->IdentifyPerson(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::EnrollPersonResponse>
IdentityClient::enrollPerson(const EnrollPersonInput& input) const
{
  if (input.image.empty())
    return std::nullopt;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::EnrollPersonRequest request;
  request.set_image(input.image);
  request.set_camera_id(input.cameraId);
  request.set_capture_snapshot(input.captureSnapshot);

  argus::identity::v1::EnrollPersonResponse response;
  if (const grpc::Status status =
          stub_->EnrollPerson(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

bool IdentityClient::touchPerson(int64_t personId, int64_t at) const
{
  if (personId <= 0)
    return false;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::TouchPersonRequest request;
  request.set_person_id(personId);
  request.set_at(at);

  argus::identity::v1::TouchPersonResponse response;
  if (const grpc::Status status =
          stub_->TouchPerson(&context, request, &response);
      !status.ok())
    return false;
  return response.updated();
}

bool IdentityClient::promotePerson(const PromotePersonInput& input) const
{
  if (input.personId <= 0 || input.accessToken.empty())
    return false;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);
  context.AddMetadata("authorization", "Bearer " + input.accessToken);
  if (!input.deviceHash.empty())
    context.AddMetadata("x-argus-device", input.deviceHash);

  argus::identity::v1::PromotePersonRequest request;
  request.set_person_id(input.personId);

  argus::identity::v1::PromotePersonResponse response;
  if (const grpc::Status status =
          stub_->PromotePerson(&context, request, &response);
      !status.ok())
    return false;
  return response.promoted();
}

bool IdentityClient::tagPerson(const TagPersonInput& input) const
{
  if (input.personId <= 0)
    return false;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::TagPersonRequest request;
  request.set_person_id(input.personId);
  for (const auto& tag : input.tags)
    request.add_tags(tag);
  request.set_source(input.source);
  request.set_observation(input.observation);

  argus::identity::v1::TagPersonResponse response;
  if (const grpc::Status status =
          stub_->TagPerson(&context, request, &response);
      !status.ok())
    return false;
  return true;
}

std::optional<std::vector<std::string>> IdentityClient::personTags(
    int64_t personId) const
{
  if (personId <= 0)
    return std::nullopt;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::PersonTagsRequest request;
  request.set_person_id(personId);

  argus::identity::v1::PersonTagsResponse response;
  if (const grpc::Status status =
          stub_->GetPersonTags(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return std::vector<std::string>(response.tags().begin(),
                                  response.tags().end());
}

std::optional<PersonProfile> IdentityClient::getPerson(
    int64_t personId) const
{
  if (personId <= 0)
    return std::nullopt;

  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  argus::identity::v1::GetPersonRequest request;
  request.set_person_id(personId);

  argus::identity::v1::GetPersonResponse response;
  if (const grpc::Status status =
          stub_->GetPerson(&context, request, &response);
      !status.ok() || !response.has_person())
    return std::nullopt;

  const auto& person = response.person();
  PersonProfile identity;
  identity.personId = person.person_id();
  if (person.has_user_id())
    identity.userId = person.user_id();
  identity.name = person.name();
  identity.alias = person.alias();
  identity.observation = person.observation();
  identity.role = person.role();
  identity.tags.assign(person.tags().begin(), person.tags().end());
  return identity;
}

std::optional<std::vector<int64_t>>
IdentityClient::listNotifiableUsers() const
{
  grpc::ClientContext context;
  argus::sdk::setDeadline(context, kCallTimeoutMs);
  argus::sdk::addFleetSecret(context, fleetSecret_);

  const argus::identity::v1::ListNotifiableUsersRequest request;

  argus::identity::v1::ListNotifiableUsersResponse response;
  if (const grpc::Status status =
          stub_->ListNotifiableUsers(&context, request, &response);
      !status.ok())
    return std::nullopt;

  std::vector<int64_t> userIds(response.user_ids().begin(),
                               response.user_ids().end());
  return userIds;
}
