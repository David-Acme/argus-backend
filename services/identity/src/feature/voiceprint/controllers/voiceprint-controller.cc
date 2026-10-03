#include "voiceprint-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <errors/response-exception.hxx>
#include <feature/voiceprint/dtos/create-voiceprint-challenge-dto.hxx>
#include <feature/voiceprint/dtos/enroll-voiceprint-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-challenge-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-sample-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-status-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-verify-dto.hxx>
#include <http/api-response.hxx>
#include <identity/identity-errors.hxx>
#include <utility>

namespace
{

const ErrorDefinition& refusalOf(VoiceprintOutcome outcome)
{
  switch (outcome) {
    case VoiceprintOutcome::Unavailable:
      return IdentityErrors::VoiceprintUnavailable;
    case VoiceprintOutcome::NotEnrolled:
      return IdentityErrors::VoiceprintNotFound;
    case VoiceprintOutcome::Stale:
      return IdentityErrors::VoiceprintStale;
    case VoiceprintOutcome::SampleTooShort:
      return IdentityErrors::VoiceSampleTooShort;
    case VoiceprintOutcome::SampleTooNoisy:
      return IdentityErrors::VoiceSampleTooNoisy;
    case VoiceprintOutcome::SampleClipped:
      return IdentityErrors::VoiceSampleClipped;
    case VoiceprintOutcome::SamplesInconsistent:
      return IdentityErrors::VoiceSamplesInconsistent;
    case VoiceprintOutcome::SampleCountInvalid:
      return IdentityErrors::VoiceSampleCountInvalid;
    case VoiceprintOutcome::AlreadyEnrolled:
      return IdentityErrors::VoiceprintAlreadyEnrolled;
    case VoiceprintOutcome::VoiceTaken:
      return IdentityErrors::VoiceAlreadyLinked;
    case VoiceprintOutcome::ChallengeInvalid:
      return IdentityErrors::VoiceprintChallengeInvalid;
    case VoiceprintOutcome::ConsentRequired:
      return IdentityErrors::VoiceprintConsentRequired;
    case VoiceprintOutcome::Forbidden:
      return IdentityErrors::VoiceprintForbidden;
    case VoiceprintOutcome::FaceNotVerified:
      return IdentityErrors::VoiceprintFaceNotVerified;
    case VoiceprintOutcome::UserNotFound:
      return IdentityErrors::UserNotFound;
    case VoiceprintOutcome::SampleInvalid:
    case VoiceprintOutcome::Ok:
      break;
  }
  return IdentityErrors::VoiceSampleInvalid;
}

void requireOk(VoiceprintOutcome outcome)
{
  if (outcome != VoiceprintOutcome::Ok)
    throw ResponseException(refusalOf(outcome));
}

bool sampleVerdict(VoiceprintOutcome outcome)
{
  switch (outcome) {
    case VoiceprintOutcome::Ok:
    case VoiceprintOutcome::SampleInvalid:
    case VoiceprintOutcome::SampleTooShort:
    case VoiceprintOutcome::SampleTooNoisy:
    case VoiceprintOutcome::SampleClipped:
      return true;
    default:
      return false;
  }
}

VoiceprintActor actorOf(const drogon::HttpRequestPtr& req)
{
  const auto& attributes = req->getAttributes();
  const auto& jwt = attributes->get<JwtContext>(AuthContext::kJwtKey);
  const auto& device = attributes->get<DeviceContext>(AuthContext::kDeviceKey);
  return {.userId = jwt.sub, .role = jwt.role, .deviceHash = device.deviceHash};
}

EncodedVoice wav(std::string bytes)
{
  return {.bytes = std::move(bytes),
          .encoding = VoiceEncoding::Wav,
          .sampleRate = 0};
}

struct ChallengeAnswerInput
{
  const VoiceprintChallengeResult& result;
  const IdentityVoiceprintConfig& config;
};

Json::Value challengeAnswer(const ChallengeAnswerInput& input)
{
  requireOk(input.result.outcome);
  return ResponseVoiceprintChallengeDto{.challenge = input.result.challenge,
                                        .consentVersion = std::string(
                                            kVoiceprintConsentVersion),
                                        .samplesRequired =
                                            input.config.samplesRequired,
                                        .minSpeechSeconds =
                                            input.config.minSpeechSeconds}
      .toJson();
}

Json::Value statusAnswer(const VoiceprintEnrollResult& result)
{
  requireOk(result.outcome);
  return ResponseVoiceprintStatusDto{.status = result.status}.toJson();
}

}

drogon::Task<VoiceprintSampleCheck>
VoiceprintController::takeSample(const SampleInput& input) const
{
  if (input.body.challengeId.empty())
    co_return co_await service_.checkSample(wav(input.body.wav()));
  co_return co_await service_.stageSample(
      {.actor = input.actor,
       .subjectId = input.subjectId,
       .challengeId = input.body.challengeId,
       .position = static_cast<int>(input.body.phrase),
       .sample = wav(input.body.wav())});
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::status(drogon::HttpRequestPtr req)
{
  const auto result = co_await service_.status(actorOf(req).userId);
  requireOk(result.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintStatusDto{.status = result.status}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::challenge(drogon::HttpRequestPtr req)
{
  const auto body =
      CreateVoiceprintChallengeDto::fromJson(*req->getJsonObject());
  const auto actor = actorOf(req);
  const VoiceprintChallengeRequest request{.actor = actor,
                                           .subjectId = actor.userId,
                                           .lang = body.language()};
  const auto result = co_await service_.createChallenge(request);
  co_return ApiResponse::ok(
      challengeAnswer({.result = result, .config = service_.config()}));
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::sample(drogon::HttpRequestPtr req)
{
  const auto body = VoiceSampleDto::fromJson(*req->getJsonObject());
  const auto actor = actorOf(req);
  const SampleInput input{.actor = actor,
                          .subjectId = actor.userId,
                          .body = body};
  const auto check = co_await takeSample(input);
  if (!sampleVerdict(check.outcome))
    requireOk(check.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintSampleDto{.check = check}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::enroll(drogon::HttpRequestPtr req)
{
  const auto body = EnrollVoiceprintDto::fromJson(*req->getJsonObject());
  const auto actor = actorOf(req);
  const VoiceprintFinalizeRequest request{.actor = actor,
                                          .subjectId = actor.userId,
                                          .consent = body.consent,
                                          .consentVersion = body.consentVersion,
                                          .challengeId = body.challengeId,
                                          .faceImage = {}};
  co_return ApiResponse::ok(statusAnswer(co_await service_.finalize(request)));
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::verify(drogon::HttpRequestPtr req)
{
  const auto body = VoiceSampleDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.verify(
      {.userId = actorOf(req).userId, .sample = wav(body.wav())});
  requireOk(result.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintVerifyDto{.result = result}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::remove(drogon::HttpRequestPtr req)
{
  const auto actor = actorOf(req);
  const VoiceprintDeleteRequest request{.actor = actor,
                                        .subjectId = actor.userId};
  requireOk((co_await service_.remove(request)).outcome);
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::statusOf(drogon::HttpRequestPtr, int64_t userId)
{
  if (userId <= 0)
    throw ResponseException(IdentityErrors::InvalidUserId);
  const auto result = co_await service_.status(userId);
  requireOk(result.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintStatusDto{.status = result.status}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::challengeFor(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto body =
      CreateVoiceprintChallengeDto::fromJson(*req->getJsonObject());
  const VoiceprintChallengeRequest request{.actor = actorOf(req),
                                           .subjectId = userId,
                                           .lang = body.language()};
  const auto result = co_await service_.createChallenge(request);
  co_return ApiResponse::ok(
      challengeAnswer({.result = result, .config = service_.config()}));
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::sampleFor(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto body = VoiceSampleDto::fromJson(*req->getJsonObject());
  const SampleInput input{.actor = actorOf(req),
                          .subjectId = userId,
                          .body = body};
  const auto check = co_await takeSample(input);
  if (!sampleVerdict(check.outcome))
    requireOk(check.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintSampleDto{.check = check}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::enrollFor(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto body = EnrollVoiceprintDto::fromJson(*req->getJsonObject());
  const VoiceprintFinalizeRequest request{.actor = actorOf(req),
                                          .subjectId = userId,
                                          .consent = body.consent,
                                          .consentVersion = body.consentVersion,
                                          .challengeId = body.challengeId,
                                          .faceImage = body.faceImage()};
  co_return ApiResponse::ok(statusAnswer(co_await service_.finalize(request)));
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::removeFor(drogon::HttpRequestPtr req, int64_t userId)
{
  const VoiceprintDeleteRequest request{.actor = actorOf(req),
                                        .subjectId = userId};
  requireOk((co_await service_.remove(request)).outcome);
  co_return ApiResponse::noContent();
}
