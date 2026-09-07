#include "health-controller.hxx"

#include <shared/wrapper/api-response/api-response.hxx>

#include <chrono>

namespace
{
const std::chrono::steady_clock::time_point kStartTime =
    std::chrono::steady_clock::now();
} // namespace

HealthController::HealthController(HealthStatus status)
    : status_(std::move(status))
{
}

drogon::Task<drogon::HttpResponsePtr>
HealthController::health(drogon::HttpRequestPtr)
{
  Json::Value info(Json::objectValue);
  info["service"] = status_.serviceName;
  info["uptimeSeconds"] =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    kStartTime)
          .count();
  info["homeConnected"] = status_.homeConnected && status_.homeConnected();
  info["activeStreams"] =
      status_.activeStreams ? status_.activeStreams() : 0;
  if (status_.pushQueued)
    info["pushQueued"] = Json::Value::Int64(status_.pushQueued());
  if (status_.pushReceived)
    info["pushReceived"] = Json::Value::Int64(status_.pushReceived());
  if (status_.pushDropped)
    info["pushDropped"] = Json::Value::Int64(status_.pushDropped());
  if (status_.pushForwarded)
    info["pushForwarded"] = Json::Value::Int64(status_.pushForwarded());
  co_return ApiResponse::ok(info);
}
