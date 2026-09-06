#include "health-controller.hxx"

#include <shared/wrapper/api-response/api-response.hxx>

#include <chrono>

namespace
{
const std::chrono::steady_clock::time_point kStartTime =
    std::chrono::steady_clock::now();
}

Json::Value HealthController::info(double uptimeSeconds)
{
  Json::Value info(Json::objectValue);
  info["service"] = "argus-tts";
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
  co_return ApiResponse::ok(info(uptime));
}
