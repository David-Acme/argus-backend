#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class PresenceService;

class PresenceController
    : public drogon::HttpController<PresenceController, false>
{
public:
  explicit PresenceController(const PresenceService* service);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(PresenceController::list, "/guard/presence", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);

private:
  const PresenceService* service_{nullptr};
};
