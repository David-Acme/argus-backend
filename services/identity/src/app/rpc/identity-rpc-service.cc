#include "identity-rpc-service.hxx"

#include <algorithm>

#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <identity/identity-errors.hxx>
#include <map>
#include <memory>
#include <optional>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/socket-emit-dto.hxx>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <shared/vocabulary/person-status.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>
#include <auth/user-role.hxx>

namespace
{
void fillPrivacy(argus::identity::v1::PrivacyChoices& target,
                 const PrivacyState& state)
{
  target.set_notice_version(static_cast<uint32_t>(state.noticeVersion));
  target.set_decided(state.decided);
  target.set_presence(state.effective.presence);
  target.set_face_cameras(state.effective.faceCameras);
  target.set_voice_learning(state.effective.voiceLearning);
  target.set_camera_audio(state.effective.cameraAudio);
}
}

namespace
{

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

}

IdentityRpcService::IdentityRpcService(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

bool IdentityRpcService::fleetAuthorized(
    const grpc::CallbackServerContext* context) const
{
  if (dependencies_.fleetSecret.empty())
    return true;
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == argus::client::kFleetSecretKey) {
      return argus::client::constantTimeEquals(
          std::string(value.begin(), value.end()), dependencies_.fleetSecret);
    }
  }
  return false;
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
      std::shared_ptr<drogon::orm::Transaction> transaction;
      try {
        transaction =
            co_await db_transaction::begin(DbService::identityClient());

        const auto before =
            co_await userRepository_.findById(userId, transaction.get());
        if (!before) {
          db_transaction::rollback(transaction);
          reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                       "user not found"));
          co_return;
        }

        auto user = co_await userRepository_.update(
            userId,
            {.name = name,
             .lastName = std::nullopt,
             .role = std::nullopt,
             .isActive = std::nullopt,
             .client = transaction.get()});
        if (user.id <= 0) {
          db_transaction::rollback(transaction);
          reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                       "user not found"));
          co_return;
        }

        std::vector<int64_t> recipients{user.id};
        for (const auto& recipient :
             co_await userRepository_.findAll(transaction.get())) {
          if (recipient.role == UserRole::Owner ||
              recipient.role == UserRole::Guard)
            recipients.push_back(recipient.id);
        }
        if (const auto* sink = identity_change::getSink()) {
          co_await sink->publishUsersAudit({
              .recordId = user.id,
              .tableName = TableName::User,
              .before = before->toJson(),
              .after = user.toJson(),
              .userIds = std::move(recipients),
              .client = transaction.get(),
          });
        }

        if (!co_await db_transaction::Commit(std::move(transaction)))
          throw ResponseException(IdentityErrors::ChangeNotRecorded);

        responseWriter->mutable_user()->set_user_id(user.id);
        responseWriter->mutable_user()->set_name(user.name);
        responseWriter->mutable_user()->set_lang(user.lang);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        db_transaction::rollback(transaction);
        LOG_WARN << "Identity RPC: UpdateUser failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

argus::identity::v1::RegisterUserOutcome
registerUserOutcome(EnrollmentOutcome outcome)
{
  switch (outcome) {
  case EnrollmentOutcome::Enrolled:
    return argus::identity::v1::REGISTER_USER_ENROLLED;
  case EnrollmentOutcome::AlreadyRegistered:
    return argus::identity::v1::REGISTER_USER_ALREADY_REGISTERED;
  case EnrollmentOutcome::NotPaired:
    return argus::identity::v1::REGISTER_USER_NOT_PAIRED;
  case EnrollmentOutcome::FaceExtractionFailed:
    return argus::identity::v1::REGISTER_USER_FACE_EXTRACTION_FAILED;
  case EnrollmentOutcome::FaceAlreadyRegistered:
    return argus::identity::v1::REGISTER_USER_FACE_ALREADY_REGISTERED;
  case EnrollmentOutcome::FaceNotRecognized:
    return argus::identity::v1::REGISTER_USER_FACE_NOT_RECOGNIZED;
  case EnrollmentOutcome::InvitationRequired:
    return argus::identity::v1::REGISTER_USER_INVITATION_REQUIRED;
  case EnrollmentOutcome::InvitationInvalid:
    return argus::identity::v1::REGISTER_USER_INVITATION_INVALID;
  case EnrollmentOutcome::OwnerAlreadyExists:
    return argus::identity::v1::REGISTER_USER_OWNER_ALREADY_EXISTS;
  case EnrollmentOutcome::FaceIndexFailed:
    return argus::identity::v1::REGISTER_USER_FACE_INDEX_FAILED;
  case EnrollmentOutcome::AccountDisabled:
    return argus::identity::v1::REGISTER_USER_ACCOUNT_DISABLED;
  }
  return argus::identity::v1::REGISTER_USER_OUTCOME_UNSPECIFIED;
}

grpc::ServerUnaryReactor* IdentityRpcService::RegisterUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::RegisterUserRequest* request,
    argus::identity::v1::RegisterUserResponse* response)
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

  EnrollmentInput input;
  input.image = image;
  input.name = request->name();
  input.invitationToken = request->invitation_token();
  input.lang = request->lang();
  input.deviceHash = request->device_hash();

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, input,
                                        responseWriter]() {
    drogon::async_run([this, reactor, input,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto result = co_await enrollmentService_.registerUser(input);
        responseWriter->set_outcome(registerUserOutcome(result.outcome));
        if (result.userId > 0) {
          auto* user = responseWriter->mutable_user();
          user->set_user_id(result.userId);
          user->set_name(result.name);
          user->set_lang(result.lang);
          user->set_last_name(result.lastName);
          user->set_role(userRoleToString(result.role));
          user->set_is_active(true);
        }
        if (result.personId > 0)
          responseWriter->set_person_id(result.personId);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: RegisterUser failed: " << e.what();
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
              fillPrivacy(*payload->mutable_privacy(),
                          co_await privacyGate_.stateFor(user->id));
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

namespace
{
std::optional<std::pair<int64_t, float>> searchFace(
    const std::optional<FaceService::FaceResult>& face)
{
  if (!face)
    return std::nullopt;
  return FaceService::instance().faceDb().search(face->embedding.data());
}
}

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

  const bool forCamera =
      request->purpose() == argus::identity::v1::IDENTIFY_PURPOSE_CAMERA;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, image, forCamera,
                                        responseWriter]() {
    drogon::async_run([this, reactor, image, forCamera,
                       responseWriter]() -> drogon::Task<void> {
      try {
        const auto face =
            co_await FaceService::instance().extractImageAsync(image);
        responseWriter->set_face_found(face.has_value());
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
          if (user && !user->isActive)
            responseWriter->set_account_disabled(true);
          if (user && user->isActive) {
            responseWriter->set_role(userRoleToString(user->role));
            if (!forCamera ||
                (co_await privacyGate_.effectiveFor(user->id)).faceCameras) {
              responseWriter->set_user_id(user->id);
              responseWriter->set_name(user->name);
              responseWriter->set_last_name(user->lastName);
            }
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
          std::optional<FaceService::FaceResult> face;
          PersonSchema person;
          std::shared_ptr<drogon::orm::Transaction> transaction;
          int64_t faceEmbeddingId = 0;
          try {
            face = co_await FaceService::instance().extractImageAsync(image);
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

            transaction =
                co_await db_transaction::begin(DbService::identityClient());

            person = co_await personRepository_.create(
                {.userId = std::nullopt,
                 .name = "",
                 .alias = "",
                 .observation = "",
                 .status = PersonStatus::Candidate,
                 .client = transaction.get()});
            if (person.id <= 0) {
              db_transaction::rollback(transaction);
              reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                           "person insert failed"));
              co_return;
            }

            const std::string embedding(
                reinterpret_cast<const char*>(face->embedding.data()),
                face->embedding.size() * sizeof(float));
            const auto row = co_await faceEmbeddingRepository_.create(
                {.personId = person.id,
                 .embedding = embedding,
                 .angleLabel = "frontal",
                 .quality = 1.0,
                 .model = std::string(kFaceModelId),
                 .client = transaction.get()});
            if (row.id > 0)
              faceEmbeddingId = row.id;

            if (captureSnapshot)
              co_await personSnapshotRepository_.store(
                  {.personId = person.id,
                   .image = image,
                   .client = transaction.get()});

            SocketEmitDto emit;
            emit.operation = SyncOperation::Add;
            emit.option = TableName::Person;
            emit.obj = person.toJson();
            if (const auto* sink = identity_change::getSink())
              co_await sink->emitModule({.table = TableName::Person,
                                         .body = emit,
                                         .client = transaction.get()});

            if (!co_await db_transaction::Commit(std::move(transaction)))
              throw ResponseException(IdentityErrors::ChangeNotRecorded);
          }
          catch (const std::exception& e) {
            db_transaction::rollback(transaction);
            LOG_WARN << "Identity RPC: EnrollPerson failed: " << e.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
            co_return;
          }

          if (faceEmbeddingId > 0) {
            co_await BlockingTask<void>([embedding = face->embedding,
                                         personId = person.id,
                                         faceEmbeddingId]() {
              FaceService::instance().faceDb().insert(
                  {.embedding = embedding.data(),
                   .personId = personId,
                   .faceEmbeddingId = faceEmbeddingId});
            });
          }

          LOG_INFO << "Identity RPC: enrolled person " << person.id
                   << " from camera " << cameraId;
          responseWriter->set_person_id(person.id);
          responseWriter->set_created(true);
          responseWriter->set_confidence(face->confidence);
          reactor->Finish(grpc::Status::OK);
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
          const bool named =
              !person->userId ||
              (co_await privacyGate_.effectiveFor(*person->userId)).faceCameras;
          if (person->userId && named)
            payload->set_user_id(*person->userId);
          if (named) {
            payload->set_name(person->name);
            payload->set_alias(person->alias);
            payload->set_observation(person->observation);
          }
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
          std::optional<PersonSchema> before;
          bool promoted = false;
          std::shared_ptr<drogon::orm::Transaction> transaction;
          try {
            const auto auth = dependencies_.auth;
            if (!auth) {
              reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "auth verdict unavailable"));
              co_return;
            }
            const auto verdict = co_await BlockingTask<
                std::optional<argus::auth::v1::ValidateTokenResponse>>(
                [auth, token, device]() {
                  return auth->validateToken({.accessToken = token,
                                              .deviceHash = device,
                                              .hasDeviceContext =
                                                  !device.empty(),
                                              .origin = {}});
                });
            if (!verdict) {
              reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                           "auth verdict unavailable"));
              co_return;
            }
            if (!verdict->valid()) {
              reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "invalid access token"));
              co_return;
            }
            const int64_t actorId = verdict->user().user_id();
            if (userRoleFromString(verdict->user().role()) != UserRole::Owner) {
              reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                           "owner role required"));
              co_return;
            }

            transaction =
                co_await db_transaction::begin(DbService::identityClient());

            before = co_await personRepository_.findById(personId,
                                                         transaction.get());
            if (!before) {
              db_transaction::rollback(transaction);
              reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                           "person not found"));
              co_return;
            }
            promoted = co_await personRepository_.promote(personId,
                                                          transaction.get());
            if (promoted) {
              const auto after = co_await personRepository_.findById(
                  personId, transaction.get());
              if (after) {
                if (const auto* sink = identity_change::getSink()) {
                  co_await sink->publishModuleAudit(
                      {.recordId = personId,
                       .tableName = TableName::Person,
                       .before = before->toJson(),
                       .after = after->toJson(),
                       .actorId = actorId,
                       .client = transaction.get()});
                }
              }
            }

            if (!co_await db_transaction::Commit(std::move(transaction)))
              throw ResponseException(IdentityErrors::ChangeNotRecorded);

            responseWriter->set_promoted(promoted);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            db_transaction::rollback(transaction);
            LOG_WARN << "Identity RPC: PromotePerson failed: " << e.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
          }
          co_return;
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::ListPrivacy(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ListPrivacyRequest* request,
    argus::identity::v1::ListPrivacyResponse* response)
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
        const auto states = co_await privacyGate_.statesByUser();
        const auto household = co_await privacyGate_.household();
        auto* allowed = responseWriter->mutable_household();
        allowed->set_notice_version(static_cast<uint32_t>(kPrivacyNoticeVersion));
        allowed->set_decided(true);
        allowed->set_presence(household.allowed.presence);
        allowed->set_face_cameras(household.allowed.faceCameras);
        allowed->set_voice_learning(household.allowed.voiceLearning);
        allowed->set_camera_audio(household.allowed.cameraAudio);
        allowed->set_visitor_recognition(household.visitorRecognition);
        for (const auto& user : co_await userRepository_.findAll()) {
          if (!user.isActive)
            continue;
          auto* entry = responseWriter->add_users();
          entry->set_user_id(user.id);
          const auto found = states.find(user.id);
          fillPrivacy(*entry->mutable_choices(),
                      found == states.end() ? PrivacyState{} : found->second);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ListPrivacy failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::ListUsers(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::ListUsersRequest* request,
    argus::identity::v1::ListUsersResponse* response)
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
        auto users = co_await userRepository_.findAll();
        std::ranges::sort(users, {}, &UserSchema::id);
        const auto states = co_await privacyGate_.statesByUser();
        for (const auto& user : users) {
          auto* payload = responseWriter->add_users();
          payload->set_user_id(user.id);
          payload->set_name(user.name);
          payload->set_lang(user.lang);
          payload->set_last_name(user.lastName);
          payload->set_role(userRoleToString(user.role));
          payload->set_is_active(user.isActive);
          const auto found = states.find(user.id);
          fillPrivacy(*payload->mutable_privacy(),
                      found == states.end() ? PrivacyState{} : found->second);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity RPC: ListUsers failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}
