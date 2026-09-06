#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>

class HealthController
    : public drogon::HttpController<HealthController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
  METHOD_LIST_END

  static Json::Value info(double uptimeSeconds);

  drogon::Task<drogon::HttpResponsePtr> health(drogon::HttpRequestPtr req);
};
