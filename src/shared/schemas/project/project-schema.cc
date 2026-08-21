#include "project-schema.hxx"

ProjectSchema::ProjectSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  ownerId = static_cast<int64_t>(row["owner_id"].as<long long>());
  name = row["name"].as<std::string>();
  description = row["description"].as<std::string>();
  status = row["status"].as<std::string>();
  color = row["color"].as<std::string>();
  if (!row["starts_at"].isNull())
    startsAt = static_cast<int64_t>(row["starts_at"].as<long long>());
  if (!row["target_at"].isNull())
    targetAt = static_cast<int64_t>(row["target_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value ProjectSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["ownerId"] = ownerId;
  json["name"] = name;
  json["description"] = description;
  json["status"] = status;
  json["color"] = color;
  json["startsAt"] = startsAt ? Json::Value(Json::Int64(*startsAt)) : Json::Value();
  json["targetAt"] = targetAt ? Json::Value(Json::Int64(*targetAt)) : Json::Value();
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
