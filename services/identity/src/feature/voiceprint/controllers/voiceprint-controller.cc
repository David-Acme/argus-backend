#include "voiceprint-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/voiceprint/dtos/response-voiceprint-directory-dto.hxx>
#include <http/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::directory(drogon::HttpRequestPtr)
{
  const ResponseVoiceprintDirectoryDto body{.directory =
                                                co_await service_.directory()};
  co_return ApiResponse::ok(body.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VoiceprintController::forget(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_await service_.forget({.actorId = jwt.sub, .subjectId = userId});
  co_return ApiResponse::noContent();
}
