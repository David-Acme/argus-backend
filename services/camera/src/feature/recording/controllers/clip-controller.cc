#include "clip-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <feature/recording/dtos/clip-page-dto.hxx>
#include <http/api-response.hxx>
#include <utility>

drogon::Task<drogon::HttpResponsePtr>
ClipController::list(drogon::HttpRequestPtr req, int64_t cameraId)
{
  const auto page = ClipPageDto::fromRequest(req);
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.list({.cameraId = cameraId,
                                              .requesterUserId = ctx.sub,
                                              .requesterRole = ctx.role,
                                              .beforeCreatedAt = page.beforeCreatedAt,
                                              .beforeId = page.beforeId,
                                              .limit = page.limit});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ClipController::preview(drogon::HttpRequestPtr req, int64_t cameraId,
                        int64_t clipId)
{
  if (clipId <= 0)
    throw ResponseException(CameraErrors::InvalidClipId);
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.createPreview(
      {.cameraId = cameraId,
       .clipId = clipId,
       .requesterUserId = ctx.sub,
       .requesterRole = ctx.role});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ClipController::remove(drogon::HttpRequestPtr req, int64_t cameraId,
                       int64_t clipId)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_await service_.remove({.cameraId = cameraId,
                            .clipId = clipId,
                            .requesterUserId = ctx.sub,
                            .requesterRole = ctx.role});
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
ClipController::content(drogon::HttpRequestPtr req, std::string token)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.consumePreview(
      {.token = std::move(token),
       .requesterUserId = ctx.sub,
       .requesterRole = ctx.role});
  co_return ApiResponse::ok(result.toJson());
}
