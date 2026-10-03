#include "voiceprint-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <drogon/MultiPart.h>
#include <errors/response-exception.hxx>
#include <feature/voiceprint/dtos/create-voiceprint-challenge-dto.hxx>
#include <feature/voiceprint/dtos/enroll-voiceprint-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-challenge-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-sample-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-status-dto.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-verify-dto.hxx>
#include <feature/voiceprint/dtos/voice-sample-dto.hxx>
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

VoiceprintActor actorOf(const drogon::HttpRequestPtr& req)
{
  const auto& attributes = req->getAttributes();
  const auto& jwt = attributes->get<JwtContext>(AuthContext::kJwtKey);
  const auto& device = attributes->get<DeviceContext>(AuthContext::kDeviceKey);
  return {.userId = jwt.sub, .role = jwt.role, .deviceHash = device.deviceHash};
}

drogon::MultiPartParser multipartOf(const drogon::HttpRequestPtr& req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    throw ResponseException(IdentityErrors::InvalidMultipartForm);
  return parser;
}

EncodedVoice wav(std::string bytes)
{
  return {.bytes = std::move(bytes),
          .encoding = VoiceEncoding::Wav,
          .sampleRate = 0};
}

std::vector<EncodedVoice> wavs(std::vector<std::string> samples)
{
  std::vector<EncodedVoice> voices;
  voices.reserve(samples.size());
  for (auto& sample : samples)
    voices.push_back(wav(std::move(sample)));
  return voices;
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
  const auto result = co_await service_.createChallenge(
      {.actor = actor, .subjectId = actor.userId, .lang = body.language()});
  co_return ApiResponse::ok(
      challengeAnswer({.result = result, .config = service_.config()}));
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::checkSample(drogon::HttpRequestPtr req)
{
  const auto parser = multipartOf(req);
  auto body = VoiceSampleDto::form_multipart(parser);
  const auto check = co_await service_.checkSample(wav(std::move(body.sample)));
  co_return ApiResponse::ok(
      ResponseVoiceprintSampleDto{.check = check}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::enroll(drogon::HttpRequestPtr req)
{
  const auto parser = multipartOf(req);
  auto body = EnrollVoiceprintDto::form_multipart(parser);
  const auto actor = actorOf(req);
  const auto result = co_await service_.enroll(
      {.actor = actor,
       .subjectId = actor.userId,
       .samples = wavs(std::move(body.samples)),
       .consent = true,
       .consentVersion = std::move(body.consentVersion),
       .challengeId = std::move(body.challengeId),
       .faceImage = {}});
  requireOk(result.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintStatusDto{.status = result.status}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::verify(drogon::HttpRequestPtr req)
{
  const auto parser = multipartOf(req);
  auto body = VoiceSampleDto::form_multipart(parser);
  const auto result = co_await service_.verify(
      {.userId = actorOf(req).userId, .sample = wav(std::move(body.sample))});
  requireOk(result.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintVerifyDto{.result = result}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::remove(drogon::HttpRequestPtr req)
{
  const auto actor = actorOf(req);
  const auto result =
      co_await service_.remove({.actor = actor, .subjectId = actor.userId});
  requireOk(result.outcome);
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
  const auto result = co_await service_.createChallenge(
      {.actor = actorOf(req), .subjectId = userId, .lang = body.language()});
  co_return ApiResponse::ok(
      challengeAnswer({.result = result, .config = service_.config()}));
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::enrollFor(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto parser = multipartOf(req);
  auto body = EnrollVoiceprintDto::form_multipart(parser);
  const auto result = co_await service_.enroll(
      {.actor = actorOf(req),
       .subjectId = userId,
       .samples = wavs(std::move(body.samples)),
       .consent = true,
       .consentVersion = std::move(body.consentVersion),
       .challengeId = std::move(body.challengeId),
       .faceImage = std::move(body.face)});
  requireOk(result.outcome);
  co_return ApiResponse::ok(
      ResponseVoiceprintStatusDto{.status = result.status}.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::removeFor(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto result =
      co_await service_.remove({.actor = actorOf(req), .subjectId = userId});
  requireOk(result.outcome);
  co_return ApiResponse::noContent();
}
