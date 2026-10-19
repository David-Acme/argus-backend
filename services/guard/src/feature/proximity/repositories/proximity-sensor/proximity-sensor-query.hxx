#pragma once

#include <feature/proximity/vocabulary/proximity-kinds.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace proximity_sensor_query
{

inline constexpr std::string_view SELECT_SENSORS =
    "SELECT id, name, room, model, source, host, port, entity_id, camera_id, pan_offset_deg, created_at "
    "FROM guard_proximity_sensor WHERE deleted_at IS NULL ORDER BY id";

inline constexpr std::string_view INSERT_SENSOR =
    "INSERT INTO guard_proximity_sensor (name, room, model, source, host, port, entity_id, camera_id, "
    "pan_offset_deg, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view DELETE_SENSOR =
    "UPDATE guard_proximity_sensor SET deleted_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

}

struct ProximitySensorRow
{
  int64_t id{0};
  std::string name;
  std::string room;
  ProximitySensorModel model{ProximitySensorModel::Ld2410};
  ProximitySensorSource source{ProximitySensorSource::Esphome};
  std::string host;
  int64_t port{80};
  std::string entityId;
  std::optional<int64_t> cameraId;
  std::optional<double> panOffsetDeg;
  int64_t createdAt{0};
};

struct ProximitySensorCreateInput
{
  std::string name;
  std::string room;
  ProximitySensorModel model{ProximitySensorModel::Ld2410};
  ProximitySensorSource source{ProximitySensorSource::Esphome};
  std::string host;
  int64_t port{80};
  std::string entityId;
  std::optional<int64_t> cameraId;
  std::optional<double> panOffsetDeg;
  int64_t now{0};
};
