#include "camera-fallback-log-repository.hxx"

#include <sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

using namespace camera_fallback_log_query;

drogon::Task<bool>
CameraFallbackLogRepository::log(const CameraFallbackLogInput& input) const
{
  auto client = DbService::client();
  if (!client)
    co_return false;
  try {
    const auto result = co_await client->execSqlCoro(
        INSERT_FALLBACK_EVENT.data(), input.cameraId, input.rule,
        input.severity, fallbackDropReasonToString(input.reason),
        input.createdAt);
    co_return result.affectedRows() > 0;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera fallback log write failed: " << error.what();
    co_return false;
  }
}

drogon::Task<int64_t>
CameraFallbackLogRepository::purgeOlderThan(int64_t olderThan) const
{
  auto client = DbService::client();
  if (!client)
    co_return 0;
  try {
    const auto result = co_await client->execSqlCoro(
        PURGE_FALLBACK_EVENTS.data(), olderThan);
    co_return result.affectedRows();
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera fallback log purge failed: " << error.what();
    co_return 0;
  }
}
