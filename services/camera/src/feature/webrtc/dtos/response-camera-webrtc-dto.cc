#include "response-camera-webrtc-dto.hxx"

Json::Value ResponseCameraWebRtcDto::toJson() const
{
  Json::Value out(Json::objectValue);
  out["type"] = "answer";
  out["sdp"] = sdp;
  out["quality"] = cameraStreamToString(stream);
  out["audio"] = audio;
  return out;
}
