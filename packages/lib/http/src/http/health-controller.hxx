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

// What /health reports; providers are called per request.
struct HealthStatus
{
  std::string serviceName;
  std::vector<std::pair<std::string, std::function<Json::Value()>>> extras;
};

// The /health surface shared by every service.
class HealthController
    : public drogon::HttpController<HealthController, false>
{
public:
  explicit HealthController(HealthStatus status);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
  METHOD_LIST_END

  // The payload without the ApiResponse envelope; tests pin its shape.
  static Json::Value info(const std::string& serviceName,
                          double uptimeSeconds);

  drogon::Task<drogon::HttpResponsePtr> health(drogon::HttpRequestPtr req);

private:
  HealthStatus status_;
};
