#include "zone-schema.hxx"

ZoneSchema::ZoneSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  cameraId = static_cast<int64_t>(row["camera_id"].as<long long>());
  name = row["name"].as<std::string>();
  points = row["points"].as<std::string>();
  zoneType = zoneTypeFromString(row["zone_type"].as<std::string>());
  color = row["color"].as<std::string>();
  isEnabled = row["is_enabled"].as<int>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value ZoneSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["cameraId"] = cameraId;
  json["name"] = name;
  json["points"] = points;
  json["zoneType"] = zoneTypeToString(zoneType);
  json["color"] = color;
  json["isEnabled"] = isEnabled;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Int64(*updatedAt) : Json::nullValue;
  json["deletedAt"] = deletedAt ? Json::Int64(*deletedAt) : Json::nullValue;
  return json;
}
