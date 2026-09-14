#pragma once

#include <operator/zone-source.hxx>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Reads enabled zones from camera.db and caches them for refreshMs.
class ZoneProvider final : public IZoneSource
{
public:
  ZoneProvider(int64_t refreshMs, std::unique_ptr<IZoneSource> fallback);

  std::vector<OperatorZone> forCamera(int64_t cameraId) override;

private:
  std::vector<OperatorZone> loadFromDb();

  int64_t refreshMs_;
  std::unique_ptr<IZoneSource> fallback_;
  std::mutex mutex_;
  int64_t loadedAtMs_{0};
  bool dbOk_{false};
  std::vector<OperatorZone> cached_;
};
