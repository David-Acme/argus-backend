#include "camera-clip-schema.hxx"

CameraClipSchema::CameraClipSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<int64_t>());
  cameraId = static_cast<int64_t>(row["camera_id"].as<int64_t>());
  trigger = cameraClipTriggerFromString(row["trigger"].as<std::string>());
  objectKey = row["object_key"].as<std::string>();
  contentType = row["content_type"].as<std::string>();
  codec = row["codec"].as<std::string>();
  startedAt = static_cast<int64_t>(row["started_at"].as<int64_t>());
  durationMs = static_cast<int64_t>(row["duration_ms"].as<int64_t>());
  byteSize = static_cast<int64_t>(row["byte_size"].as<int64_t>());
  createdAt = static_cast<int64_t>(row["created_at"].as<int64_t>());
  expiresAt = static_cast<int64_t>(row["expires_at"].as<int64_t>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<int64_t>());
}

Json::Value CameraClipSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["cameraId"] = cameraId;
  json["trigger"] = cameraClipTriggerToString(trigger);
  json["contentType"] = contentType;
  json["codec"] = codec;
  json["startedAt"] = startedAt;
  json["durationMs"] = durationMs;
  json["byteSize"] = byteSize;
  json["createdAt"] = createdAt;
  json["expiresAt"] = expiresAt;
  return json;
}
