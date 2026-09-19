#include <operator/zone-provider.hxx>

#include <shared/services/sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <utility>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

ZoneProvider::ZoneProvider(int64_t refreshMs,
                           std::unique_ptr<IZoneSource> fallback)
    : refreshMs_(refreshMs), fallback_(std::move(fallback))
{
}

std::vector<OperatorZone> ZoneProvider::forCamera(int64_t cameraId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t stamp = nowMs();
  if (!dbOk_ || stamp - loadedAtMs_ >= refreshMs_) {
    try {
      cached_ = loadFromDb();
      loadedAtMs_ = stamp;
      dbOk_ = true;
    }
    catch (const std::exception& e) {
      LOG_WARN << "Zone provider: camera.db read failed (" << e.what()
               << "); using configured zones";
      dbOk_ = false;
    }
  }
  if (!dbOk_)
    return fallback_ ? fallback_->forCamera(cameraId)
                     : std::vector<OperatorZone>{};

  std::vector<OperatorZone> zones;
  for (const auto& zone : cached_) {
    if (zone.cameraId == cameraId)
      zones.push_back(zone);
  }
  return zones;
}

std::vector<OperatorZone> ZoneProvider::loadFromDb()
{
  std::vector<OperatorZone> zones;
  const auto rows = DbService::client()->execSqlSync(
      "SELECT camera_id, name, zone_type, points FROM zone "
      "WHERE deleted_at IS NULL AND is_enabled = 1");
  for (const auto& row : rows) {
    OperatorZone zone;
    zone.cameraId = row["camera_id"].as<int64_t>();
    zone.name = row["name"].as<std::string>();
    zone.kind = row["zone_type"].as<std::string>();
    zone.points = parseZonePoints(row["points"].as<std::string>());
    if (zone.cameraId > 0 && !zone.name.empty() && zone.points.size() >= 3)
      zones.push_back(std::move(zone));
  }
  return zones;
}
