#include "vlm-controller.hxx"

#include <vlm/describe-dto.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <drogon/drogon.h>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <string>

drogon::Task<drogon::HttpResponsePtr>
VlmController::describe(drogon::HttpRequestPtr req)
{
  if (!service_.isLoaded())
    co_return ApiResponse::error(503, "VLM_NOT_LOADED",
                                 "Vision engine is not loaded");

  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return ApiResponse::error(400, "BAD_REQUEST",
                                 "Body must be a JSON object");

  const auto body = DescribeImageDto::fromJson(*req->getJsonObject());

  const std::string jpeg = drogon::utils::base64Decode(body.imageB64);
  if (jpeg.empty()) {
    Json::Value fields(Json::objectValue);
    fields["image_b64"] = "decodes to no bytes";
    co_return ApiResponse::validationError(fields);
  }

  const cv::Mat raw(1, static_cast<int>(jpeg.size()), CV_8UC1,
                    const_cast<char*>(jpeg.data()));
  const cv::Mat bgr = cv::imdecode(raw, cv::IMREAD_COLOR);
  if (bgr.empty()) {
    Json::Value fields(Json::objectValue);
    fields["image_b64"] = "not a decodable image";
    co_return ApiResponse::validationError(fields);
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string caption =
      co_await service_.describeMatAsync(bgr, body.prompt, 0);
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "VLM describe: bytes=" << jpeg.size()
           << " camera_id=" << body.cameraId << " ms=" << static_cast<int>(ms);

  Json::Value info(Json::objectValue);
  info["caption"] = std::move(caption);
  co_return ApiResponse::ok(info);
}

drogon::Task<drogon::HttpResponsePtr>
VlmController::engine(drogon::HttpRequestPtr)
{
  Json::Value info(Json::objectValue);
  info["loaded"] = service_.isLoaded();
  info["maxInputPx"] = service_.maxInputPx();
  info["defaultMaxTokens"] = service_.defaultMaxTokens();
  co_return ApiResponse::ok(info);
}

void VlmController::initEngine()
{
  service_.init();
}

void VlmController::shutdownEngine()
{
  service_.shutdown();
}

bool VlmController::isEngineLoaded() const
{
  return service_.isLoaded();
}
