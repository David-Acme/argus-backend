#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct HealthStatus
{
  std::string serviceName;
  std::vector<std::pair<std::string, std::function<Json::Value()>>> extras;
};

class HealthController
    : public drogon::HttpController<HealthController, false>
{
public:
  explicit HealthController(HealthStatus status);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
  METHOD_LIST_END

  static Json::Value info(const std::string& serviceName,
                          double uptimeSeconds);

  drogon::Task<drogon::HttpResponsePtr> health(drogon::HttpRequestPtr req);

private:
  HealthStatus status_;
};
