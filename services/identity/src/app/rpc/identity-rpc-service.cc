#include "identity-rpc-service.hxx"

#include <algorithm>

#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpc/grpc-server-identity.hxx>
#include "identity-callers.hxx"
#include <identity/identity-errors.hxx>
#include <map>
#include <memory>
#include <optional>
#include <shared/services/face/face-service.hxx>
#include <identity/person-category.hxx>
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

void IdentityRpcService::finishInternal(const InternalFailure& failure)
{
  LOG_WARN << "Identity RPC: " << failure.call << " failed: " << failure.error.what();
  failure.reactor->Finish(
      grpc::Status(grpc::StatusCode::INTERNAL, "identity could not complete the call"));
}

IdentityRpcService::IdentityRpcService(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

grpc::ServerUnaryReactor*
IdentityRpcService::refuseCaller(grpc::CallbackServerContext* context,
                                 argus::client::CallerSet allowed) const
{
  const auto admission = dependencies_.gate->admit(context, allowed);
  if (admission.admitted())
    return nullptr;
  auto* reactor = context->DefaultReactor();
  reactor->Finish(argus::client::FleetCallerGate::refusal(admission.verdict));
  return reactor;
}

grpc::ServerUnaryReactor* IdentityRpcService::UpdateUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::UpdateUserRequest* request,
    argus::identity::v1::UpdateUserResponse* response)
{
  if (auto* refused = refuseCaller(context, identity_callers::kUpdateUser))
    return refused;

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
        const auto user = co_await userService_.rename({.userId = userId, .name = name});
        if (!user) {
          reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                       "user not found"));
          co_return;
        }
        responseWriter->mutable_user()->set_user_id(user->id);
        responseWriter->mutable_user()->set_name(user->name);
        responseWriter->mutable_user()->set_lang(user->lang);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        finishInternal({.reactor = reactor, .call = "UpdateUser", .error = e});
      }
      co_return;
    });
  });
  return reactor;
}

namespace
{
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
  case EnrollmentOutcome::LivenessFailed:
    return argus::identity::v1::REGISTER_USER_LIVENESS_FAILED;
  case EnrollmentOutcome::LivenessUnavailable:
    return argus::identity::v1::REGISTER_USER_LIVENESS_UNAVAILABLE;
  case EnrollmentOutcome::FaceQualityInsufficient:
    return argus::identity::v1::REGISTER_USER_FACE_QUALITY_INSUFFICIENT;
  }
  return argus::identity::v1::REGISTER_USER_OUTCOME_UNSPECIFIED;
}
}

grpc::ServerUnaryReactor* IdentityRpcService::RegisterUser(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::RegisterUserRequest* request,
    argus::identity::v1::RegisterUserResponse* response)
{
  if (auto* refused = refuseCaller(context, identity_callers::kRegisterUser))
    return refused;

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
        finishInternal({.reactor = reactor, .call = "RegisterUser", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
            finishInternal({.reactor = reactor, .call = "GetUser", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
        finishInternal({.reactor = reactor, .call = "ListPersons", .error = e});
      }
      co_return;
    });
  });
  return reactor;
}

namespace
{
void fillMatch(argus::identity::v1::IdentifyPersonResponse& response,
               const HouseholdMatch& match)
{
  response.set_trusted(match.trusted);
  if (match.accountDisabled)
    response.set_account_disabled(true);
  if (match.role)
    response.set_role(userRoleToString(*match.role));
  if (match.userId) {
    response.set_user_id(*match.userId);
    response.set_name(match.name);
    response.set_last_name(match.lastName);
  }
}
}

drogon::Task<void>
IdentityRpcService::identifySighting(const IdentifySightingInput& input)
{
  auto* response = input.response;
  const auto result = co_await visitorRecognition_.observe(
      {.image = input.image, .cameraId = input.cameraId, .observedAt = input.observedAt});
  response->set_face_found(result.faceFound);
  if (!result.faceFound)
    co_return;
  response->set_face_quality(std::string(face_quality::verdictToString(result.quality)));
  const auto outcome = result.decision.outcome;
  if (outcome == SightingOutcome::Household) {
    response->set_matched(true);
    response->set_person_id(result.decision.personId);
    response->set_confidence(result.decision.score);
    fillMatch(*response, co_await personService_.describeMatch(
                             {.personId = result.decision.personId, .forCamera = true}));
    co_return;
  }
  if (outcome != SightingOutcome::Visitor && outcome != SightingOutcome::NewVisitor)
    co_return;
  response->set_matched(true);
  response->set_person_id(result.decision.personId);
  response->set_confidence(result.decision.score);
  response->set_visitor(true);
  response->set_created(result.created);
  response->set_visits(static_cast<int32_t>(result.visits));
  if (result.visitorNumber)
    response->set_visitor_number(*result.visitorNumber);
  response->set_category(std::string(personCategoryToString(result.category)));
  response->set_trusted(result.known && result.named &&
                        personCategoryLowersRisk(result.category));
  if (result.named) {
    const auto person = co_await personRepository_.findById(result.decision.personId);
    if (person)
      response->set_name(person->name);
  }
}

drogon::Task<void> IdentityRpcService::identifyForCamera(const IdentifyInput& input)
{
  auto* response = input.response;
  const auto face = co_await FaceService::instance().extractImageAsync(input.image);
  response->set_face_found(face.has_value());
  if (!face)
    co_return;
  const auto match = co_await BlockingTask<std::optional<std::pair<int64_t, float>>>(
      [embedding = face->embedding]() {
        return FaceService::instance().faceDb().search(embedding.data());
      });
  if (!match)
    co_return;
  response->set_matched(true);
  response->set_person_id(match->first);
  response->set_confidence(match->second);
  fillMatch(*response, co_await personService_.describeMatch(
                           {.personId = match->first, .forCamera = true}));
}

drogon::Task<void> IdentityRpcService::identifyForSignIn(const IdentifyInput& input)
{
  auto* response = input.response;
  const auto signIn = co_await signInService_.identify(input.image);
  response->set_face_check(std::string(face_check::statusToString(signIn.check)));
  response->set_face_found(signIn.check != FaceCheckStatus::NoFace &&
                           signIn.check != FaceCheckStatus::Undecodable &&
                           signIn.check != FaceCheckStatus::Unavailable);
  if (!signIn.match)
    co_return;
  response->set_matched(true);
  response->set_person_id(signIn.match->personId);
  response->set_confidence(signIn.match->score);
  fillMatch(*response, co_await personService_.describeMatch(
                           {.personId = signIn.match->personId, .forCamera = false}));
}

grpc::ServerUnaryReactor* IdentityRpcService::IdentifyPerson(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::IdentifyPersonRequest* request,
    argus::identity::v1::IdentifyPersonResponse* response)
{
  if (auto* refused = refuseCaller(context, identity_callers::kIdentifyPerson))
    return refused;

  const std::string image = request->image();
  if (image.empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "image is required"));
    return reactor;
  }

  const bool forCamera =
      request->purpose() == argus::identity::v1::IDENTIFY_PURPOSE_CAMERA;
  const std::optional<argus::identity::v1::SightingContext> sighting =
      forCamera && request->has_sighting()
          ? std::optional<argus::identity::v1::SightingContext>(request->sighting())
          : std::nullopt;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, image, forCamera, sighting,
                                        responseWriter]() {
    drogon::async_run([this, reactor, image, forCamera, sighting,
                       responseWriter]() -> drogon::Task<void> {
      try {
        if (sighting)
          co_await identifySighting({.image = image,
                                     .cameraId = sighting->camera_id(),
                                     .observedAt = sighting->observed_at(),
                                     .response = responseWriter});
        else if (forCamera)
          co_await identifyForCamera({.image = image, .response = responseWriter});
        else
          co_await identifyForSignIn({.image = image, .response = responseWriter});
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        finishInternal({.reactor = reactor, .call = "IdentifyPerson", .error = e});
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
  if (auto* refused = refuseCaller(context, identity_callers::kCameraSighting))
    return refused;

  const std::string image = request->image();
  if (image.empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "image is required"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, image, cameraId, responseWriter]() {
        drogon::async_run([this, reactor, image, cameraId,
                           responseWriter]() -> drogon::Task<void> {
          try {
            if (!(co_await privacyGate_.household()).visitorRecognition) {
              reactor->Finish(grpc::Status(
                  grpc::StatusCode::FAILED_PRECONDITION,
                  "recognition of recurring visitors is turned off"));
              co_return;
            }
            const auto result = co_await visitorRecognition_.observe(
                {.image = image,
                 .cameraId = cameraId,
                 .observedAt = static_cast<int64_t>(std::time(nullptr))});
            if (!result.faceFound) {
              reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "no face detected in the crop"));
              co_return;
            }
            const auto outcome = result.decision.outcome;
            if (outcome == SightingOutcome::Household ||
                outcome == SightingOutcome::Visitor ||
                outcome == SightingOutcome::NewVisitor) {
              responseWriter->set_person_id(result.decision.personId);
              responseWriter->set_confidence(result.decision.score);
            }
            responseWriter->set_created(result.created);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            finishInternal({.reactor = reactor, .call = "EnrollPerson", .error = e});
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
  if (auto* refused = refuseCaller(context, identity_callers::kCameraSighting))
    return refused;

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
            responseWriter->set_updated(co_await personService_.touch(personId, at));
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            finishInternal({.reactor = reactor, .call = "TouchPerson", .error = e});
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
  if (auto* refused = refuseCaller(context, identity_callers::kGuardCuration))
    return refused;

  const int64_t personId = request->person_id();
  if (personId <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "person_id is required"));
    return reactor;
  }

  PersonTagRequest tagRequest{
      .personId = personId,
      .tags = std::vector<std::string>(request->tags().begin(), request->tags().end()),
      .source = request->source().empty() ? "llm" : request->source(),
      .observation = request->observation()};
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop(
      [this, reactor, tagRequest = std::move(tagRequest), responseWriter]() {
        drogon::async_run([this, reactor, tagRequest,
                           responseWriter]() -> drogon::Task<void> {
          try {
            responseWriter->set_added(co_await personService_.tag(tagRequest));
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            finishInternal({.reactor = reactor, .call = "TagPerson", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
        finishInternal({.reactor = reactor, .call = "ListNotifiableUsers", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
        for (const auto& tag : co_await personService_.tags(personId))
          responseWriter->add_tags(tag);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        finishInternal({.reactor = reactor, .call = "GetPersonTags", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
        const auto person = co_await personService_.describe(personId);
        if (person) {
          auto* payload = responseWriter->mutable_person();
          payload->set_person_id(person->personId);
          if (person->userId)
            payload->set_user_id(*person->userId);
          if (person->named) {
            payload->set_name(person->name);
            payload->set_alias(person->alias);
            payload->set_observation(person->observation);
          }
          payload->set_trusted(person->trusted);
          if (person->role)
            payload->set_role(userRoleToString(*person->role));
          if (person->visitor) {
            payload->set_category(std::string(personCategoryToString(person->category)));
            payload->set_visits(static_cast<int32_t>(person->visits));
            payload->set_first_seen_at(person->firstSeenAt);
            payload->set_last_seen_at(person->lastSeenAt);
            if (person->visitorNumber)
              payload->set_visitor_number(*person->visitorNumber);
            auto* pattern = payload->mutable_pattern();
            for (const int day : person->pattern.weekdays)
              pattern->add_weekdays(day);
            if (person->pattern.usualHour)
              pattern->set_usual_hour(*person->pattern.usualHour);
            pattern->set_visits_considered(person->pattern.visitsConsidered);
          }
          for (const auto& tag : person->tags)
            payload->add_tags(tag);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        finishInternal({.reactor = reactor, .call = "GetPerson", .error = e});
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
  if (auto* refused = refuseCaller(context, identity_callers::kGuardCuration))
    return refused;

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
            if (userRoleFromString(verdict->user().role()) != UserRole::Owner) {
              reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                           "owner role required"));
              co_return;
            }
            const auto promoted = co_await personService_.promote(
                {.personId = personId, .actorId = verdict->user().user_id()});
            if (!promoted) {
              reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND,
                                           "person not found"));
              co_return;
            }
            responseWriter->set_promoted(*promoted);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& e) {
            finishInternal({.reactor = reactor, .call = "PromotePerson", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
        finishInternal({.reactor = reactor, .call = "ListPrivacy", .error = e});
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
  if (auto* refused = refuseCaller(context, {}))
    return refused;

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
        finishInternal({.reactor = reactor, .call = "ListUsers", .error = e});
      }
      co_return;
    });
  });
  return reactor;
}
