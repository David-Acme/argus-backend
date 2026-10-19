#pragma once

#include <feature/rtc/services/rtc-token-service.hxx>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class RtcController : public drogon::HttpController<RtcController, false>
{
public:
  explicit RtcController(RtcTokenServiceInput input);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(RtcController::token, "/rtc/token", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(RtcController::decline, "/rtc/decline", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(RtcController::ice, "/rtc/ice", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> token(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> decline(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> ice(drogon::HttpRequestPtr req);

private:
  RtcTokenService service_;
};
