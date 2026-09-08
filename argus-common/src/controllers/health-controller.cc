#include "health-controller.hxx"

#include <shared/wrapper/api-response/api-response.hxx>

#include <chrono>

namespace
{
// Process start, not controller construction: uptime must survive a
// controller being registered late.
const std::chrono::steady_clock::time_point kStartTime =
    std::chrono::steady_clock::now();
}

HealthController::HealthController(HealthStatus status)
    : status_(std::move(status))
{
}

Json::Value HealthController::info(const std::string& serviceName,
                                   double uptimeSeconds)
{
  Json::Value info(Json::objectValue);
  info["service"] = serviceName;
  info["uptimeSeconds"] = uptimeSeconds;
  return info;
}

drogon::Task<drogon::HttpResponsePtr>
HealthController::health(drogon::HttpRequestPtr)
{
  const double uptime =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    kStartTime)
          .count();
  Json::Value payload = info(status_.serviceName, uptime);
  for (const auto& [key, provider] : status_.extras)
    payload[key] = provider();
  co_return ApiResponse::ok(payload);
}
