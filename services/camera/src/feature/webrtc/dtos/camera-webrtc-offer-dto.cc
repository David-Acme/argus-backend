#include "camera-webrtc-offer-dto.hxx"

#include <validation/validation_dsl.hxx>

namespace
{
constexpr std::size_t kMaxOfferBytes = 16384;
}

CameraWebRtcOfferDto CameraWebRtcOfferDto::fromJson(const Json::Value& json)
{
  CameraWebRtcOfferDto dto;
  dto.sdp = json.get("sdp", "").asString();
  if (json["quality"].isString())
    dto.quality = json["quality"].asString();

  START_VALIDATION(CameraWebRtcOfferDto, dto)
  IS_NOT_EMPTY(sdp)
  MAX_LENGTH(sdp, kMaxOfferBytes)
  IS_IN_OPTIONAL(quality, "main", "sub")
  END_VALIDATION()
  return dto;
}

CameraStream CameraWebRtcOfferDto::stream() const
{
  const auto chosen = quality ? cameraStreamFromString(*quality) : std::nullopt;
  return chosen.value_or(camera_stream_role::streamFor(CameraStreamRole::LiveView));
}
