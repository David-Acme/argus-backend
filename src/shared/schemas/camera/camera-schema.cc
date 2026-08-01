#include "camera-schema.hxx"

CameraSchema::CameraSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  name = row["name"].as<std::string>();
  manufacturer = row["manufacturer"].as<std::string>();
  model = row["model"].as<std::string>();
  ip = row["ip"].as<std::string>();
  port = row["port"].as<int32_t>();
  username = row["username"].as<std::string>();
  password = row["password"].as<std::string>();
  recordMode = cameraRecordModeFromString(row["record_mode"].as<std::string>());
  if (!row["retention_days"].isNull())
    retentionDays = static_cast<int64_t>(row["retention_days"].as<long long>());
  capabilities = row["capabilities"].as<std::string>();
  config = row["config"].as<std::string>();
  isEnabled = row["is_enabled"].as<int>() != 0;
  isOnline = row["is_online"].as<int>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value CameraSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["name"] = name;
  json["manufacturer"] = manufacturer;
  json["model"] = model;
  json["ip"] = ip;
  json["port"] = port;
  json["username"] = username;
  json["password"] = password;
  json["recordMode"] = cameraRecordModeToString(recordMode);
  json["retentionDays"] =
      retentionDays ? Json::Value(Json::Int64(*retentionDays)) : Json::Value();
  json["capabilities"] = capabilities;
  json["config"] = config;
  json["isEnabled"] = isEnabled;
  json["isOnline"] = isOnline;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
