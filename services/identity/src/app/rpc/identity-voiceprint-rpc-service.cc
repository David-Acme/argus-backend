#include "identity-voiceprint-rpc-service.hxx"

#include <auth/user-role.hxx>
#include <drogon/drogon.h>
#include <grpc/grpc-server-identity.hxx>
#include <runtime/blocking-task.hxx>
#include <string_view>
#include <utility>

namespace
{

namespace v1 = argus::identity::v1;

v1::VoiceprintOutcome wireOutcome(VoiceprintOutcome outcome)
{
  switch (outcome) {
    case VoiceprintOutcome::Ok:
      return v1::VOICEPRINT_OK;
    case VoiceprintOutcome::Unavailable:
      return v1::VOICEPRINT_UNAVAILABLE;
    case VoiceprintOutcome::NotEnrolled:
      return v1::VOICEPRINT_NOT_ENROLLED;
    case VoiceprintOutcome::Stale:
      return v1::VOICEPRINT_STALE;
    case VoiceprintOutcome::SampleInvalid:
      return v1::VOICEPRINT_SAMPLE_INVALID;
    case VoiceprintOutcome::SampleTooShort:
      return v1::VOICEPRINT_SAMPLE_TOO_SHORT;
    case VoiceprintOutcome::SampleTooNoisy:
      return v1::VOICEPRINT_SAMPLE_TOO_NOISY;
    case VoiceprintOutcome::SampleClipped:
      return v1::VOICEPRINT_SAMPLE_CLIPPED;
    case VoiceprintOutcome::SamplesInconsistent:
      return v1::VOICEPRINT_SAMPLES_INCONSISTENT;
    case VoiceprintOutcome::SampleCountInvalid:
      return v1::VOICEPRINT_SAMPLE_COUNT_INVALID;
    case VoiceprintOutcome::AlreadyEnrolled:
      return v1::VOICEPRINT_ALREADY_ENROLLED;
    case VoiceprintOutcome::VoiceTaken:
      return v1::VOICEPRINT_VOICE_TAKEN;
    case VoiceprintOutcome::ChallengeInvalid:
      return v1::VOICEPRINT_CHALLENGE_INVALID;
    case VoiceprintOutcome::ConsentRequired:
      return v1::VOICEPRINT_CONSENT_REQUIRED;
    case VoiceprintOutcome::Forbidden:
      return v1::VOICEPRINT_FORBIDDEN;
    case VoiceprintOutcome::FaceNotVerified:
      return v1::VOICEPRINT_FACE_NOT_VERIFIED;
    case VoiceprintOutcome::UserNotFound:
      return v1::VOICEPRINT_USER_NOT_FOUND;
  }
  return v1::VOICEPRINT_OUTCOME_UNSPECIFIED;
}

void fillStatus(const VoiceprintStatusView& view, v1::VoiceprintStatus* out)
{
  out->set_enrolled(view.enrolled);
  out->set_stale(view.stale);
  out->set_model(view.model);
  out->set_sample_count(view.sampleCount);
  out->set_enrolled_at(view.enrolledAt);
  if (view.enrolled)
    out->set_method(voiceprintMethodToString(view.method));
  out->set_consent_version(view.consentVersion);
}

EncodedVoice pcmOf(const v1::VoiceClip& clip)
{
  return {.bytes = clip.pcm16(),
          .encoding = VoiceEncoding::Pcm16,
          .sampleRate = clip.sample_rate()};
}

}

IdentityVoiceprintRpcService::IdentityVoiceprintRpcService(
    Dependencies dependencies)
    : dependencies_(std::move(dependencies)), service_(dependencies_.voiceprint)
{
}

bool IdentityVoiceprintRpcService::fleetAuthorized(
    const grpc::CallbackServerContext* context) const
{
  if (dependencies_.fleetSecret.empty())
    return true;
  return argus::client::constantTimeEquals(
      argus::client::metadata(context, argus::client::kFleetSecretKey),
      dependencies_.fleetSecret);
}

IdentityVoiceprintRpcService::SessionCredentials
IdentityVoiceprintRpcService::credentialsOf(
    const grpc::CallbackServerContext* context)
{
  constexpr std::string_view kBearer = "Bearer ";
  const std::string header = argus::client::metadata(context, "authorization");
  return {.accessToken = header.starts_with(kBearer)
                             ? header.substr(kBearer.size())
                             : std::string(),
          .deviceHash = argus::client::metadata(context, "x-argus-device")};
}

drogon::Task<IdentityVoiceprintRpcService::VerifiedActor>
IdentityVoiceprintRpcService::verifyActor(SessionCredentials credentials) const
{
  if (credentials.accessToken.empty())
    co_return VerifiedActor{
        .status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                               "access token required"),
        .actor = {}};
  const auto auth = dependencies_.auth;
  if (!auth)
    co_return VerifiedActor{.status =
                                grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                             "auth verdict unavailable"),
                            .actor = {}};
  const auto verdict = co_await BlockingTask<
      std::optional<argus::auth::v1::ValidateTokenResponse>>(
      [auth, credentials]() {
        return auth->validateToken(
            {.accessToken = credentials.accessToken,
             .deviceHash = credentials.deviceHash,
             .hasDeviceContext = !credentials.deviceHash.empty()});
      });
  if (!verdict)
    co_return VerifiedActor{.status =
                                grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                             "auth verdict unavailable"),
                            .actor = {}};
  if (!verdict->valid())
    co_return VerifiedActor{.status =
                                grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                             "invalid access token"),
                            .actor = {}};
  co_return VerifiedActor{.status = grpc::Status::OK,
                          .actor = {.userId = verdict->user().user_id(),
                                    .role = userRoleFromString(
                                        verdict->user().role()),
                                    .deviceHash = credentials.deviceHash}};
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::refuse(grpc::CallbackServerContext* context,
                                     const grpc::Status& status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(status);
  return reactor;
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::dispatch(grpc::CallbackServerContext* context,
                                       Work work)
{
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop([reactor, work = std::move(work)]() {
    drogon::async_run([reactor, work]() -> drogon::Task<void> {
      try {
        reactor->Finish(co_await work());
      }
      catch (const std::exception& error) {
        LOG_WARN << "Identity voiceprint RPC failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, error.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* IdentityVoiceprintRpcService::CreateChallenge(
    grpc::CallbackServerContext* context,
    const v1::CreateVoiceprintChallengeRequest* request,
    v1::CreateVoiceprintChallengeResponse* response)
{
  if (!fleetAuthorized(context))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                        "fleet secret missing or wrong"));
  if (request->user_id() <= 0)
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "user_id is required"));
  auto credentials = credentialsOf(context);
  return dispatch(context,
                  [this, request, response,
                   credentials]() -> drogon::Task<grpc::Status> {
                    const auto verified = co_await verifyActor(credentials);
                    if (!verified.status.ok())
                      co_return verified.status;
                    std::optional<VoiceLang> lang;
                    if (request->has_lang())
                      lang = voiceLangFromString(request->lang());
                    const VoiceprintChallengeRequest
                        challengeRequest{.actor = verified.actor,
                                         .subjectId = request->user_id(),
                                         .lang = lang};
                    const auto result =
                        co_await service_.createChallenge(challengeRequest);
                    response->set_outcome(wireOutcome(result.outcome));
                    if (result.outcome == VoiceprintOutcome::Ok) {
                      response->set_challenge_id(result.challenge.challengeId);
                      for (const auto& phrase : result.challenge.phrases)
                        response->add_phrases(phrase);
                      response->set_expires_at(result.challenge.expiresAt);
                      response->set_lang(
                          voiceLangToString(result.challenge.lang));
                    }
                    response->set_consent_version(
                        std::string(kVoiceprintConsentVersion));
                    co_return grpc::Status::OK;
                  });
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::Enroll(grpc::CallbackServerContext* context,
                                     const v1::EnrollVoiceprintRequest* request,
                                     v1::EnrollVoiceprintResponse* response)
{
  if (!fleetAuthorized(context))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                        "fleet secret missing or wrong"));
  if (request->user_id() <= 0)
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "user_id is required"));
  auto credentials = credentialsOf(context);
  return dispatch(context,
                  [this, request, response,
                   credentials]() -> drogon::Task<grpc::Status> {
                    const auto verified = co_await verifyActor(credentials);
                    if (!verified.status.ok())
                      co_return verified.status;
                    std::vector<EncodedVoice> samples;
                    samples.reserve(
                        static_cast<size_t>(request->samples_size()));
                    for (const auto& clip : request->samples())
                      samples.push_back(pcmOf(clip));
                    VoiceprintEnrollRequest
                        enrollRequest{.actor = verified.actor,
                                      .subjectId = request->user_id(),
                                      .samples = std::move(samples),
                                      .consent = request->consent(),
                                      .consentVersion =
                                          request->consent_version(),
                                      .challengeId = request->challenge_id(),
                                      .faceImage = request->has_face_image()
                                                       ? request->face_image()
                                                       : std::string()};
                    const auto result =
                        co_await service_.enroll(std::move(enrollRequest));
                    response->set_outcome(wireOutcome(result.outcome));
                    fillStatus(result.status, response->mutable_status());
                    if (result.failedSample)
                      response->set_failed_sample(*result.failedSample);
                    co_return grpc::Status::OK;
                  });
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::Verify(grpc::CallbackServerContext* context,
                                     const v1::VerifyVoiceprintRequest* request,
                                     v1::VerifyVoiceprintResponse* response)
{
  if (!fleetAuthorized(context))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                        "fleet secret missing or wrong"));
  if (request->user_id() <= 0 || !request->has_sample())
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "user_id and sample are required"));
  return dispatch(context,
                  [this, request, response]() -> drogon::Task<grpc::Status> {
                    VoiceprintVerifyRequest
                        verifyRequest{.userId = request->user_id(),
                                      .sample = pcmOf(request->sample())};
                    const auto result =
                        co_await service_.verify(std::move(verifyRequest));
                    response->set_outcome(wireOutcome(result.outcome));
                    response->set_matched(result.matched);
                    response->set_score(result.score);
                    response->set_threshold(result.threshold);
                    co_return grpc::Status::OK;
                  });
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::Identify(grpc::CallbackServerContext* context,
                                       const v1::IdentifyVoiceRequest* request,
                                       v1::IdentifyVoiceResponse* response)
{
  if (!fleetAuthorized(context))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                        "fleet secret missing or wrong"));
  if (!request->has_sample())
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "sample is required"));
  return dispatch(context,
                  [this, request, response]() -> drogon::Task<grpc::Status> {
                    const auto result =
                        co_await service_.identify(pcmOf(request->sample()));
                    response->set_outcome(wireOutcome(result.outcome));
                    response->set_matched(result.matched);
                    response->set_score(result.score);
                    response->set_threshold(result.threshold);
                    if (result.matched) {
                      response->set_user_id(result.userId);
                      response->set_name(result.name);
                      if (result.role)
                        response->set_role(userRoleToString(*result.role));
                      if (result.personId)
                        response->set_person_id(*result.personId);
                    }
                    co_return grpc::Status::OK;
                  });
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::Delete(grpc::CallbackServerContext* context,
                                     const v1::DeleteVoiceprintRequest* request,
                                     v1::DeleteVoiceprintResponse* response)
{
  if (!fleetAuthorized(context))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                        "fleet secret missing or wrong"));
  if (request->user_id() <= 0)
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "user_id is required"));
  auto credentials = credentialsOf(context);
  return dispatch(context,
                  [this, request, response,
                   credentials]() -> drogon::Task<grpc::Status> {
                    const auto verified = co_await verifyActor(credentials);
                    if (!verified.status.ok())
                      co_return verified.status;
                    const VoiceprintDeleteRequest
                        deleteRequest{.actor = verified.actor,
                                      .subjectId = request->user_id()};
                    const auto result = co_await service_.remove(deleteRequest);
                    response->set_outcome(wireOutcome(result.outcome));
                    response->set_deleted(result.deleted);
                    co_return grpc::Status::OK;
                  });
}

grpc::ServerUnaryReactor* IdentityVoiceprintRpcService::GetStatus(
    grpc::CallbackServerContext* context,
    const v1::GetVoiceprintStatusRequest* request,
    v1::GetVoiceprintStatusResponse* response)
{
  if (!fleetAuthorized(context))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                        "fleet secret missing or wrong"));
  if (request->user_id() <= 0)
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "user_id is required"));
  return dispatch(context,
                  [this, request, response]() -> drogon::Task<grpc::Status> {
                    const auto result =
                        co_await service_.status(request->user_id());
                    response->set_outcome(wireOutcome(result.outcome));
                    response->set_available(result.status.available);
                    fillStatus(result.status, response->mutable_status());
                    co_return grpc::Status::OK;
                  });
}
