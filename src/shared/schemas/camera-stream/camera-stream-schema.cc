#include "camera-stream-schema.hxx"

CameraStreamSchema::CameraStreamSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  cameraId = static_cast<int64_t>(row["camera_id"].as<long long>());
  label = row["label"].as<std::string>();
  url = row["url"].as<std::string>();
  resolution = row["resolution"].as<std::string>();
  fps = row["fps"].as<int>();
  codec = row["codec"].as<std::string>();
  isPrimary = row["is_primary"].as<int>() != 0;
  isEnabled = row["is_enabled"].as<int>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value CameraStreamSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["cameraId"] = cameraId;
  json["label"] = label;
  json["url"] = url;
  json["resolution"] = resolution;
  json["fps"] = fps;
  json["codec"] = codec;
  json["isPrimary"] = isPrimary;
  json["isEnabled"] = isEnabled;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Int64(*updatedAt) : Json::nullValue;
  json["deletedAt"] = deletedAt ? Json::Int64(*deletedAt) : Json::nullValue;
  return json;
}
