#pragma once

#include <feature/heartbeat/services/heartbeat-service.hxx>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <memory>

class HeartbeatController
    : public drogon::HttpController<HeartbeatController, false>
{
public:
  explicit HeartbeatController(std::shared_ptr<const HeartbeatService> service);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HeartbeatController::heartbeat, "/sync/heartbeat", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> heartbeat(drogon::HttpRequestPtr req);

private:
  std::shared_ptr<const HeartbeatService> service_;
};
