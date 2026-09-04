#include "health-controller.hxx"

#include <chrono>

namespace
{
const std::chrono::steady_clock::time_point kStartTime =
    std::chrono::steady_clock::now();
}

Json::Value HealthController::envelope(double uptimeSeconds)
{
  Json::Value info(Json::objectValue);
  info["service"] = "argus-gateway";
  info["uptimeSeconds"] = uptimeSeconds;

  Json::Value body(Json::objectValue);
  body["status"] = "ok";
  body["info"] = info;
  return body;
}

drogon::Task<drogon::HttpResponsePtr>
HealthController::health(drogon::HttpRequestPtr)
{
  const double uptime =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    kStartTime)
          .count();
  auto response = drogon::HttpResponse::newHttpJsonResponse(envelope(uptime));
  response->setStatusCode(drogon::k200OK);
  co_return response;
}
