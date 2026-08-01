#include "person-schema.hxx"

PersonSchema::PersonSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["user_id"].isNull())
    userId = static_cast<int64_t>(row["user_id"].as<long long>());
  name = row["name"].as<std::string>();
  alias = row["alias"].as<std::string>();
  observation = row["observation"].as<std::string>();
  firstSeenAt = static_cast<int64_t>(row["first_seen_at"].as<long long>());
  lastSeenAt = static_cast<int64_t>(row["last_seen_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value PersonSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["userId"] = userId ? Json::Value(Json::Int64(*userId)) : Json::Value();
  json["name"] = name;
  json["alias"] = alias;
  json["observation"] = observation;
  json["firstSeenAt"] = Json::Int64(firstSeenAt);
  json["lastSeenAt"] = Json::Int64(lastSeenAt);
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
