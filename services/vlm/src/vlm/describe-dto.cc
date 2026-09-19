#include "describe-dto.hxx"

namespace
{

// Cap on the base64 JPEG body (~24 MB decoded).
constexpr size_t kMaxImageB64Length = 32 * 1024 * 1024;
constexpr size_t kMaxPromptLength = 512;
constexpr size_t kMaxCameraIdLength = 64;

} // namespace

DescribeImageDto DescribeImageDto::fromJson(const Json::Value& json)
{
  DescribeImageDto dto;
  dto.imageB64 = json.get("image_b64", "").asString();
  dto.prompt = json.get("prompt", "").asString();
  dto.cameraId = json.get("camera_id", "").asString();

  START_VALIDATION(DescribeImageDto, dto)
  IS_NOT_EMPTY(imageB64)
  MAX_LENGTH(imageB64, kMaxImageB64Length)
  IS_BASE64(imageB64)
  MAX_LENGTH(prompt, kMaxPromptLength)
  MAX_LENGTH(cameraId, kMaxCameraIdLength)
  END_VALIDATION()
  return dto;
}
