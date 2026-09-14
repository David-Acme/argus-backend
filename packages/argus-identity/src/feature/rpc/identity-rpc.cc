#include "identity-rpc.hxx"

#include <ctime>
#include <grpc-client-base.hxx>
#include <map>
#include <drogon/drogon.h>
#include <optional>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/socket/sync-change.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

namespace
{

// Row scoping: x-argus-user must carry the updated user's id.
std::optional<int64_t> scopedUserId(const grpc::CallbackServerContext* context)
{
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == "x-argus-user") {
      try {
        return std::stoll(std::string(value.begin(), value.end()));
      }
      catch (const std::exception&) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::string deviceHash(const grpc::CallbackServerContext* context)
{
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == "x-argus-device")
      return std::string(value.begin(), value.end());
  }
  return {};
}

// Bearer access token forwarded by the SDK; the server verifies it itself.
std::string accessToken(const grpc::CallbackServerContext* context)
{
  for (const auto& [key, value] : context->client_metadata()) {
    if (key != "authorization")
      continue;
    const std::string header(value.begin(), value.end());
    constexpr std::string_view kPrefix = "Bearer ";
    if (header.rfind(kPrefix, 0) == 0)
      return header.substr(kPrefix.size());
  }
  return {};
}

// Secret comparison that does not return early on the first differing byte.
bool constantTimeEquals(const std::string& a, const std::string& b)
{
  if (a.size() != b.size())
    return false;
  unsigned char diff = 0;
  for (std::string::size_type i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
}

} // namespace

IdentityRpcService::IdentityRpcService(std::shared_ptr<NatsBus> bus,
                                       std::string fleetSecret)
    : bus_(std::move(bus)), fleetSecret_(std::move(fleetSecret))
{
}

bool IdentityRpcService::fleetAuthorized(
    const grpc::CallbackServerContext* context) const
{
  if (fleetSecret_.empty())
    return true;
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == argus::sdk::kFleetSecretKey) {
      return constantTimeEquals(std::string(value.begin(), value.end()),
                                fleetSecret_);
    }
  }
  return false;
}

void IdentityRpcService::finishRejected(const TokenRejectionInput& input)
{
  input.response->set_valid(false);
  input.response->set_reason(input.reason);
  input.reactor->Finish(grpc::Status::OK);
}

grpc::ServerUnaryReactor* IdentityRpcService::UpdateUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::UpdateUserRequest* request,
    argus::identity::v1::UpdateUserResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::optional<int64_t> scopedUser = scopedUserId(context);
  if (!scopedUser || *scopedUser != request->user_id()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "x-argus-user does not match user_id"));
    return reactor;
  }

  const int64_t userId = request->user_id();
  if (userId <= 0 || !request->has_name() || request->name().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "user_id and name are required"));
    return reactor;
  }

  const std::string name = request->name();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, userId, name,
                                        responseWriter]() {
    drogon::async_run([this, reactor, userId, name,
                       responseWriter]() -> drogon::Task<void> {
      try {
        auto user = co_await userRepository_.update(
            userId,
            {.name = name,
             .lastName = std::nullopt,
             .role = std::nullopt,
             .isActive = std::nullopt});
        if (user.id <= 0) {
          reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                       "user not found"));
          co_return;
        }

        SocketEmitDto emit;
        emit.operation = SyncOperation::Add;
        emit.option = TableName::User;
        emit.obj = user.toJson();
        if (bus_) {
          const Json::Value payload = sync_change::emitPayload(emit);
          bus_->publish(nats_subject::kSyncChange, json_util::toString(payload));
        }
        else {
          LOG_WARN << "Identity RPC: no NATS bus; user change not fanned out";
        }

        responseWriter->mutable_user()->set_user_id(user.id);
        responseWriter->mutable_user()->set_name(user.name);
        responseWriter->mutable_user()->set_lang(user.lang);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: UpdateUser failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

// Device binding keys on request-field PRESENCE, not on the hash being non-empty.
grpc::ServerUnaryReactor* IdentityRpcService::ValidateToken(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ValidateTokenRequest* request,
    argus::identity::v1::ValidateTokenResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string accessToken = request->access_token();
  const bool hasDeviceContext = request->has_device_hash();
  const std::string deviceHash =
      hasDeviceContext ? request->device_hash() : "";
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, accessToken, deviceHash,
                                        hasDeviceContext, responseWriter]() {
    drogon::async_run([this, reactor, accessToken, deviceHash,
                       hasDeviceContext,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto claims = jwtService_.verifyAccess(accessToken);
        if (claims.empty()) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = ""});
          co_return;
        }

        const auto subIt = claims.find("sub");
        int64_t userId = 0;
        if (subIt != claims.end()) {
          try {
            userId = std::stoll(subIt->second);
          }
          catch (const std::exception&) {
          }
        }
        if (userId <= 0) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = ""});
          co_return;
        }

        const auto user = co_await userRepository_.findById(userId);
        if (!user) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = ""});
          co_return;
        }

        if (!user->isActive) {
          finishRejected({.reactor = reactor,
                          .response = responseWriter,
                          .reason = "User account is disabled"});
          co_return;
        }

        int64_t expiresAt = 0;
        if (hasDeviceContext) {
          const auto rt =
              co_await refreshTokenRepository_.findByAccessToken(userId,
                                                                 accessToken);
          if (!rt) {
            finishRejected({.reactor = reactor,
                            .response = responseWriter,
                            .reason = ""});
            co_return;
          }

          expiresAt = rt->expiresAt;
          if (rt->expiresAt <= std::time(nullptr)) {
            LOG_WARN << "Refresh token expired for user " << userId;
            finishRejected({.reactor = reactor,
                            .response = responseWriter,
                            .reason = "Token expired"});
            co_return;
          }
          if (rt->deviceHash != deviceHash) {
            LOG_WARN << "Device hash mismatch for user " << userId;
            finishRejected({.reactor = reactor,
                            .response = responseWriter,
                            .reason = "Device mismatch"});
            co_return;
          }
        }

        auto* payload = responseWriter->mutable_user();
        payload->set_user_id(user->id);
        payload->set_name(user->name);
        payload->set_lang(user->lang);
        payload->set_last_name(user->lastName);
        payload->set_role(userRoleToString(user->role));
        payload->set_is_active(user->isActive);
        responseWriter->set_expires_at(expiresAt);
        responseWriter->set_valid(true);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ValidateToken failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::GetUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::GetUserRequest* request,
    argus::identity::v1::GetUserResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const int64_t userId = request->user_id();
  if (userId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "user_id is required"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, userId, responseWriter]() {
        drogon::async_run([this, reactor, userId,
                           responseWriter]() -> drogon::Task<void> {
          try {
            const auto user = co_await userRepository_.findById(userId);
            if (user) {
              auto* payload = responseWriter->mutable_user();
              payload->set_user_id(user->id);
              payload->set_name(user->name);
              payload->set_lang(user->lang);
              payload->set_last_name(user->lastName);
              payload->set_role(userRoleToString(user->role));
              payload->set_is_active(user->isActive);
            }
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            LOG_WARN << "Identity RPC: GetUser failed: " << e.what();
            reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::ListPersons(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ListPersonsRequest* request,
    argus::identity::v1::ListPersonsResponse* response)
{
  (void)request;
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter]() {
    drogon::async_run([this, reactor, responseWriter]() -> drogon::Task<void> {
      try {
        for (const auto& person :
             co_await personRepository_.findAllCatalog()) {
          auto* row = responseWriter->add_persons();
          row->set_id(person.id);
          row->set_user_id(person.userId.value_or(0));
          row->set_name(person.name);
          row->set_alias(person.alias);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ListPersons failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::CheckDeviceCredential(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::CheckDeviceCredentialRequest* request,
    argus::identity::v1::CheckDeviceCredentialResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string secretHash = request->secret_hash();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, secretHash,
                                        responseWriter]() {
    drogon::async_run([this, reactor, secretHash,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto active =
            co_await deviceCredentialRepository_.findActiveBySecretHash(
                secretHash);
        responseWriter->set_active(active.has_value());
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: CheckDeviceCredential failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

namespace
{
std::optional<std::pair<int64_t, float>> searchFace(
    const std::optional<FaceService::FaceResult>& face)
{
  if (!face)
    return std::nullopt;
  return FaceService::instance().faceDb().search(face->embedding.data());
}
} // namespace

grpc::ServerUnaryReactor* IdentityRpcService::IdentifyPerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::IdentifyPersonRequest* request,
    argus::identity::v1::IdentifyPersonResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string image = request->image();
  if (image.empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "image is required"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, image,
                                        responseWriter]() {
    drogon::async_run([this, reactor, image,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto face =
            co_await FaceService::instance().extractImageAsync(image);
        const auto match = co_await BlockingTask<
            std::optional<std::pair<int64_t, float>>>(
            [&face]() { return searchFace(face); });
        if (!match) {
          reactor->Finish(grpc::Status::OK);
          co_return;
        }

        responseWriter->set_matched(true);
        responseWriter->set_person_id(match->first);
        responseWriter->set_confidence(match->second);
        const auto person = co_await personRepository_.findById(match->first);
        if (person) {
          responseWriter->set_trusted(person->status == PersonStatus::Known);
        }
        if (person && person->userId) {
          const auto user = co_await userRepository_.findById(*person->userId);
          if (user && user->isActive) {
            responseWriter->set_user_id(user->id);
            responseWriter->set_role(userRoleToString(user->role));
            responseWriter->set_name(user->name);
          }
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: IdentifyPerson failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::EnrollPerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::EnrollPersonRequest* request,
    argus::identity::v1::EnrollPersonResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string image = request->image();
  if (image.empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "image is required"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  const bool captureSnapshot = request->capture_snapshot();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, image, cameraId, captureSnapshot, responseWriter]() {
        drogon::async_run([this, reactor, image, cameraId, captureSnapshot,
                           responseWriter]() -> drogon::Task<void> {
          try {
            const auto face =
                co_await FaceService::instance().extractImageAsync(image);
            if (!face) {
              reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "no face detected in the crop"));
              co_return;
            }
            const auto existing = co_await BlockingTask<
                std::optional<std::pair<int64_t, float>>>(
                [&face]() { return searchFace(face); });
            if (existing) {
              responseWriter->set_person_id(existing->first);
              responseWriter->set_created(false);
              responseWriter->set_confidence(existing->second);
              reactor->Finish(grpc::Status::OK);
              co_return;
            }

            const auto person = co_await personRepository_.create(
                {.userId = std::nullopt,
                 .name = "",
                 .alias = "",
                 .observation = "",
                 .status = PersonStatus::Candidate});
            if (person.id <= 0) {
              reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                           "person insert failed"));
              co_return;
            }

            if (face) {
              const std::string embedding(
                  reinterpret_cast<const char*>(face->embedding.data()),
                  face->embedding.size() * sizeof(float));
              const auto row = co_await faceEmbeddingRepository_.create(
                  {.personId = person.id,
                   .embedding = embedding,
                   .angleLabel = "frontal",
                   .quality = 1.0});
              if (row.id > 0) {
                co_await BlockingTask<void>([&face, personId = person.id,
                                             faceEmbeddingId = row.id]() {
                  FaceService::instance().faceDb().insert(
                      {.embedding = face->embedding.data(),
                       .personId = personId,
                       .faceEmbeddingId = faceEmbeddingId});
                });
              }
            }
            if (captureSnapshot)
              co_await personSnapshotRepository_.store(
                  {.personId = person.id, .image = image});

            if (bus_) {
              SocketEmitDto emit;
              emit.operation = SyncOperation::Add;
              emit.option = TableName::Person;
              emit.obj = person.toJson();
              const Json::Value payload = sync_change::emitPayload(emit);
              bus_->publish(nats_subject::kSyncChange,
                            json_util::toString(payload));
            }
            LOG_INFO << "Identity RPC: enrolled person " << person.id
                     << " from camera " << cameraId;
            responseWriter->set_person_id(person.id);
            responseWriter->set_created(true);
            responseWriter->set_confidence(face ? face->confidence : 0.0F);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            LOG_WARN << "Identity RPC: EnrollPerson failed: " << e.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::TouchPerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::TouchPersonRequest* request,
    argus::identity::v1::TouchPersonResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const int64_t personId = request->person_id();
  if (personId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "person_id is required"));
    return reactor;
  }

  const int64_t at =
      request->at() > 0 ? request->at() : static_cast<int64_t>(std::time(nullptr));
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, personId, at, responseWriter]() {
        drogon::async_run([this, reactor, personId, at,
                           responseWriter]() -> drogon::Task<void> {
          try {
            const auto person = co_await personRepository_.update(
                personId,
                {.name = std::nullopt,
                 .alias = std::nullopt,
                 .observation = std::nullopt,
                 .lastSeenAt = at});
            responseWriter->set_updated(person.id > 0);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            LOG_WARN << "Identity RPC: TouchPerson failed: " << e.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::TagPerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::TagPersonRequest* request,
    argus::identity::v1::TagPersonResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const int64_t personId = request->person_id();
  if (personId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "person_id is required"));
    return reactor;
  }

  const std::vector<std::string> tags(request->tags().begin(),
                                      request->tags().end());
  const std::string source =
      request->source().empty() ? "llm" : request->source();
  const std::string observation = request->observation();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, personId, tags, source, observation,
       responseWriter]() {
        drogon::async_run([this, reactor, personId, tags, source, observation,
                           responseWriter]() -> drogon::Task<void> {
          try {
            const int added = co_await personTagRepository_.addMany(
                {.personId = personId, .tags = tags, .source = source});
            if (!observation.empty()) {
              co_await personRepository_.update(
                  personId,
                  {.name = std::nullopt,
                   .alias = std::nullopt,
                   .observation = observation,
                   .lastSeenAt = std::nullopt});
            }
            responseWriter->set_added(added);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            LOG_WARN << "Identity RPC: TagPerson failed: " << e.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::ListNotifiableUsers(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ListNotifiableUsersRequest* request,
    argus::identity::v1::ListNotifiableUsersResponse* response)
{
  (void)request;
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter]() {
    drogon::async_run([this, reactor, responseWriter]() -> drogon::Task<void> {
      try {
        for (const int64_t userId :
             co_await userRepository_.findNotifiableIds())
          responseWriter->add_user_ids(userId);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ListNotifiableUsers failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::GetPersonTags(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::PersonTagsRequest* request,
    argus::identity::v1::PersonTagsResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const int64_t personId = request->person_id();
  if (personId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "person_id is required"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, personId,
                                        responseWriter]() {
    drogon::async_run([this, reactor, personId,
                       responseWriter]() -> drogon::Task<void> {
      try {
        for (const auto& tag :
             co_await personTagRepository_.findByPerson(personId))
          responseWriter->add_tags(tag);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: GetPersonTags failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::GetPerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::GetPersonRequest* request,
    argus::identity::v1::GetPersonResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const int64_t personId = request->person_id();
  if (personId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "person_id is required"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, personId,
                                        responseWriter]() {
    drogon::async_run([this, reactor, personId,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto person = co_await personRepository_.findById(personId);
        if (person) {
          auto* payload = responseWriter->mutable_person();
          payload->set_person_id(person->id);
          if (person->userId)
            payload->set_user_id(*person->userId);
          payload->set_name(person->name);
          payload->set_alias(person->alias);
          payload->set_observation(person->observation);
          payload->set_trusted(person->status == PersonStatus::Known);
          if (person->userId) {
            const auto user =
                co_await userRepository_.findById(*person->userId);
            if (user && user->isActive)
              payload->set_role(userRoleToString(user->role));
          }
          for (const auto& tag :
               co_await personTagRepository_.findByPerson(personId))
            payload->add_tags(tag);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: GetPerson failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::PromotePerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::PromotePersonRequest* request,
    argus::identity::v1::PromotePersonResponse* response)
{
  if (!fleetAuthorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Fleet secret missing or invalid"));
    return reactor;
  }

  const std::string token = accessToken(context);
  if (token.empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                 "owner access token required"));
    return reactor;
  }
  const std::string device = deviceHash(context);

  const int64_t personId = request->person_id();
  if (personId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "person_id is required"));
    return reactor;
  }

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, responseWriter, personId, token, device]() {
        drogon::async_run([this, reactor, responseWriter, personId, token,
                           device]() -> drogon::Task<void> {
          try {
            std::map<std::string, std::string> claims;
            try {
              claims = jwtService_.verifyAccess(token);
            }
            catch (const std::exception&) {
              reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "invalid access token"));
              co_return;
            }
            int64_t actorId = 0;
            try {
              actorId = std::stoll(claims["sub"]);
            }
            catch (const std::exception&) {
              reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "invalid token subject"));
              co_return;
            }
            const auto actorUser =
                co_await userRepository_.findById(actorId);
            if (!actorUser || !actorUser->isActive ||
                actorUser->role != UserRole::Owner) {
              reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                           "owner role required"));
              co_return;
            }
            if (!co_await refreshTokenRepository_.hasActiveSession(actorId,
                                                                  device)) {
              reactor->Finish(
                  grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                               "no active session for this owner"));
              co_return;
            }

            const auto before = co_await personRepository_.findById(personId);
            if (!before) {
              reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                           "person not found"));
              co_return;
            }
            const bool promoted =
                co_await personRepository_.promote(personId);
            if (promoted) {
              const auto after =
                  co_await personRepository_.findById(personId);
              if (after)
                co_await auditService_.publishModule(
                    {.recordId = personId,
                     .tableName = TableName::Person,
                     .before = before->toJson(),
                     .after = after->toJson(),
                     .actorId = actorId});
            }
            responseWriter->set_promoted(promoted);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            LOG_WARN << "Identity RPC: PromotePerson failed: " << e.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}
