#pragma once

#include <feature/webrtc/dtos/camera-webrtc-offer-dto.hxx>
#include <feature/webrtc/dtos/response-camera-webrtc-dto.hxx>
#include <feature/webrtc/infra/go2rtc-webrtc-gateway.hxx>
#include <feature/webrtc/services/webrtc-admission.hxx>
#include <feature/webrtc/services/webrtc-sdp.hxx>
#include <shared/repositories/camera/camera-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct CameraWebRtcRequest
{
  int64_t cameraId{0};
  CameraWebRtcOfferDto offer;
  WebRtcViewer viewer;
  bool priority{false};
};

struct WebRtcViewerCount
{
  int64_t cameraId{0};
  const Json::Value& streams;
  const std::unordered_map<int64_t, int>& hubViewers;
};

struct WebRtcViewerTally
{
  int perCamera{0};
  int total{0};
};

class CameraWebRtcService
{
public:
  drogon::Task<ResponseCameraWebRtcDto> answer(CameraWebRtcRequest request) const;

  static WebRtcViewerTally tally(const WebRtcViewerCount& count);

  [[nodiscard]] static std::vector<std::string> viewerTagsOf(int64_t userId);

private:
  static WebRtcAdmission& admission();

  CameraRepository repository_;
  Go2rtcWebRtcGateway gateway_;
};
