#include "camera-webrtc-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <http/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
CameraWebRtcController::offer(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraWebRtcOfferDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto answer = co_await service_.answer(
      {.cameraId = id,
       .offer = body,
       .viewer = {.userId = jwt.sub, .sessionId = jwt.sessionId},
       .priority = jwt.role == UserRole::Owner || jwt.role == UserRole::Resident});
  co_return ApiResponse::ok(answer.toJson());
}
