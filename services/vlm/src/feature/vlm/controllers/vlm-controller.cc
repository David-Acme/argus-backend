#include "vlm-controller.hxx"

#include <errors/response-exception.hxx>
#include <http/api-response.hxx>
#include <feature/vlm/dtos/describe-dto.hxx>
#include <feature/vlm/services/jpeg-gate.hxx>
#include <runtime/blocking-task.hxx>
#include <vlm/vlm-errors.hxx>

#include <drogon/drogon.h>

#include <chrono>
#include <string>
#include <utility>

namespace
{

struct UploadedImage
{
  std::size_t bytes{0};
  DecodedJpeg decoded;
};

const char* refusalText(JpegRefusal refusal)
{
  switch (refusal) {
    case JpegRefusal::NotJpeg:
      return "not a JPEG image";
    case JpegRefusal::TooLarge:
      return "image dimensions are too large";
    default:
      return "not a decodable image";
  }
}

}

drogon::Task<drogon::HttpResponsePtr>
VlmController::describe(drogon::HttpRequestPtr req)
{
  if (!service_.isLoaded())
    throw ResponseException(VlmErrors::VisionEngineNotLoaded);

  if (!req->getJsonError().empty() || !req->getJsonObject())
    throw ResponseException(VlmErrors::BodyNotJsonObject);

  const auto body = DescribeImageDto::fromJson(*req->getJsonObject());

  UploadedImage image = co_await BlockingTask<UploadedImage>(
      [encoded = body.imageB64] {
        const std::string jpeg = drogon::utils::base64Decode(encoded);
        if (jpeg.empty())
          return UploadedImage{};
        return UploadedImage{.bytes = jpeg.size(), .decoded = decodeCameraJpeg(jpeg)};
      },
      BlockingLane::Heavy);
  if (image.bytes == 0) {
    Json::Value fields(Json::objectValue);
    fields["image_b64"] = "decodes to no bytes";
    co_return ApiResponse::validationError(fields);
  }
  if (image.decoded.refusal != JpegRefusal::None) {
    Json::Value fields(Json::objectValue);
    fields["image_b64"] = refusalText(image.decoded.refusal);
    co_return ApiResponse::validationError(fields);
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string caption = co_await service_.describeMatAsync(
      {.bgr = std::move(image.decoded.bgr), .prompt = body.prompt, .maxTokens = 0});
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "VLM describe: bytes=" << image.bytes
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
