#include "proximity-sensor-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace proximity_sensor_query;

drogon::Task<std::vector<ProximitySensorRow>> ProximitySensorRepository::find() const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(SELECT_SENSORS));
  std::vector<ProximitySensorRow> sensors;
  sensors.reserve(rows.size());
  for (const auto& row : rows) {
    const auto model = proximitySensorModelFromString(row["model"].as<std::string>());
    const auto source = proximitySensorSourceFromString(row["source"].as<std::string>());
    if (!model || !source)
      continue;
    sensors.push_back({.id = row["id"].as<int64_t>(),
                       .name = row["name"].as<std::string>(),
                       .room = row["room"].as<std::string>(),
                       .model = *model,
                       .source = *source,
                       .host = row["host"].as<std::string>(),
                       .port = row["port"].as<int64_t>(),
                       .entityId = row["entity_id"].as<std::string>(),
                       .cameraId = row["camera_id"].isNull() ? std::nullopt
                                                             : std::optional<int64_t>(row["camera_id"].as<int64_t>()),
                       .panOffsetDeg = row["pan_offset_deg"].isNull()
                                           ? std::nullopt
                                           : std::optional<double>(row["pan_offset_deg"].as<double>()),
                       .createdAt = row["created_at"].as<int64_t>()});
  }
  co_return sensors;
}

drogon::Task<ProximitySensorRow> ProximitySensorRepository::create(const ProximitySensorCreateInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(INSERT_SENSOR), input.name, input.room, proximitySensorModelToString(input.model),
      proximitySensorSourceToString(input.source), input.host, input.port, input.entityId, input.cameraId, input.panOffsetDeg,
      input.now);
  co_return ProximitySensorRow{.id = static_cast<int64_t>(result.insertId()),
                               .name = input.name,
                               .room = input.room,
                               .model = input.model,
                               .source = input.source,
                               .host = input.host,
                               .port = input.port,
                               .entityId = input.entityId,
                               .cameraId = input.cameraId,
                               .panOffsetDeg = input.panOffsetDeg,
                               .createdAt = input.now};
}

drogon::Task<bool> ProximitySensorRepository::remove(int64_t id) const
{
  const auto result = co_await DbService::client()->execSqlCoro(std::string(DELETE_SENSOR), id);
  co_return result.affectedRows() > 0;
}
