#include "camera-patrol-schema.hxx"

#include <text/json-util.hxx>

#include <utility>

namespace
{
std::vector<std::string> stopsOf(const std::string& text)
{
  std::vector<std::string> stops;
  const Json::Value parsed = json_util::fromString(text);
  if (!parsed.isArray())
    return stops;
  stops.reserve(parsed.size());
  for (const Json::Value& stop : parsed)
    if (stop.isString())
      stops.push_back(stop.asString());
  return stops;
}
}

CameraPatrolSchema::CameraPatrolSchema(const drogon::orm::Row& row)
{
  cameraId = row["camera_id"].as<int64_t>();
  stops = stopsOf(row["stops"].as<std::string>());
  dwellSeconds = row["dwell_seconds"].as<int64_t>();
  isEnabled = row["is_enabled"].as<int>() != 0;
  if (!row["start_hour"].isNull() && !row["end_hour"].isNull())
    window = PatrolWindow{.startHour = row["start_hour"].as<int>(),
                          .endHour = row["end_hour"].as<int>()};
  createdAt = row["created_at"].as<int64_t>();
  if (!row["updated_at"].isNull())
    updatedAt = row["updated_at"].as<int64_t>();
}

Json::Value CameraPatrolSchema::toJson() const
{
  Json::Value json(Json::objectValue);
  json["cameraId"] = Json::Int64(cameraId);
  Json::Value list(Json::arrayValue);
  for (const std::string& stop : stops)
    list.append(stop);
  json["stops"] = std::move(list);
  json["dwellSeconds"] = Json::Int64(dwellSeconds);
  json["enabled"] = isEnabled;
  if (window) {
    Json::Value hours(Json::objectValue);
    hours["startHour"] = window->startHour;
    hours["endHour"] = window->endHour;
    json["window"] = std::move(hours);
  }
  else {
    json["window"] = Json::Value();
  }
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  return json;
}

std::string CameraPatrolSchema::stopsText(const std::vector<std::string>& stops)
{
  Json::Value list(Json::arrayValue);
  for (const std::string& stop : stops)
    list.append(stop);
  return json_util::toString(list);
}

ActiveCameraPatrol::ActiveCameraPatrol(const drogon::orm::Row& row)
    : patrol(row), cameraOnline(row["camera_online"].as<int>() != 0)
{
}
