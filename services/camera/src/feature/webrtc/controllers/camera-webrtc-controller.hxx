#pragma once

#include <feature/webrtc/services/camera-webrtc-service.hxx>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class CameraWebRtcController : public drogon::HttpController<CameraWebRtcController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CameraWebRtcController::offer, "/camera/{1}/webrtc", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> offer(drogon::HttpRequestPtr req, int64_t id);

private:
  CameraWebRtcService service_;
};
