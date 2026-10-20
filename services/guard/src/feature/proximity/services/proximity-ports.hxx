#pragma once

#include <feature/proximity/repositories/proximity-receiver/proximity-receiver-query.hxx>
#include <feature/proximity/repositories/proximity-sensor/proximity-sensor-query.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct BleHeard
{
  std::string uuid;
  int rssi{-100};
};

struct RadarPoint
{
  double xMm{0.0};
  double yMm{0.0};
};

struct ProximitySighting
{
  int64_t cameraId{0};
  int64_t personId{0};
  bool known{false};
  bool night{false};
  int64_t atMs{0};
};

struct ProximityCameraRoom
{
  int64_t cameraId{0};
  std::string room;
};

class ProximityProbe
{
public:
  ProximityProbe() = default;
  ProximityProbe(const ProximityProbe&) = delete;
  ProximityProbe& operator=(const ProximityProbe&) = delete;
  virtual ~ProximityProbe() = default;

  [[nodiscard]] virtual drogon::Task<std::optional<std::vector<BleHeard>>>
  readBle(ProximityReceiverRow receiver) const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<bool>> readRadar(ProximitySensorRow sensor) const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<std::vector<RadarPoint>>>
  readTargets(ProximitySensorRow sensor) const = 0;
};

class ProximityWorld
{
public:
  ProximityWorld() = default;
  ProximityWorld(const ProximityWorld&) = delete;
  ProximityWorld& operator=(const ProximityWorld&) = delete;
  virtual ~ProximityWorld() = default;

  [[nodiscard]] virtual drogon::Task<std::optional<std::vector<ProximitySighting>>> sightings(int64_t sinceMs) const = 0;

  [[nodiscard]] virtual drogon::Task<std::vector<ProximityCameraRoom>> cameraRooms() const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<std::unordered_set<int64_t>>> locatedUsers() const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<int64_t>> userOfPerson(int64_t personId) const = 0;
};
