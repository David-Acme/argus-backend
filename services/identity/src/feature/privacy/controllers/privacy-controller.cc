#include "privacy-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
#include <auth/role-access.hxx>
#include <auth/request-context.hxx>
#include <feature/privacy/dtos/response-privacy-dto.hxx>
#include <feature/privacy/dtos/update-household-privacy-dto.hxx>
#include <feature/privacy/dtos/update-privacy-dto.hxx>
#include <http/api-response.hxx>

namespace
{
bool surveillanceActive()
{
  return moduleGate().enabled(role_access::kSurveillanceModule);
}
}

drogon::Task<drogon::HttpResponsePtr>
PrivacyController::me(drogon::HttpRequestPtr req)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const ResponsePrivacyDto body{.view = co_await service_.me(jwt.sub), .surveillanceActive = surveillanceActive()};
  co_return ApiResponse::ok(body.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
PrivacyController::decide(drogon::HttpRequestPtr req)
{
  const auto dto = UpdatePrivacyDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const ResponsePrivacyDto body{.view = co_await service_.decide(
      {.userId = jwt.sub,
       .noticeVersion = dto.noticeVersion,
       .choices = {.presence = dto.presence.value_or(false),
                   .faceCameras = dto.faceCameras.value_or(false),
                   .voiceLearning = dto.voiceLearning.value_or(false),
                   .cameraAudio = dto.cameraAudio.value_or(false)}}),
                                .surveillanceActive = surveillanceActive()};
  co_return ApiResponse::ok(body.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
PrivacyController::directory(drogon::HttpRequestPtr)
{
  const ResponsePrivacyDirectoryDto body{.directory = co_await service_.directory(),
                                         .surveillanceActive = surveillanceActive()};
  co_return ApiResponse::ok(body.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
PrivacyController::household(drogon::HttpRequestPtr req)
{
  const auto dto = UpdateHouseholdPrivacyDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const ResponsePrivacyDirectoryDto body{.directory = co_await service_.updateHousehold(
      {.actorId = jwt.sub,
       .presence = dto.presence,
       .faceCameras = dto.faceCameras,
       .voiceLearning = dto.voiceLearning,
       .cameraAudio = dto.cameraAudio,
       .visitorRecognition = dto.visitorRecognition}),
                                         .surveillanceActive = surveillanceActive()};
  co_return ApiResponse::ok(body.toJson());
}
