#include "user-schema.hxx"

UserSchema::UserSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  name = row["name"].as<std::string>();
  lastName = row["last_name"].as<std::string>();
  role = userRoleFromString(row["role"].as<std::string>());
  isActive = row["is_active"].as<int>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value UserSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["name"] = name;
  json["lastName"] = lastName;
  json["role"] = userRoleToString(role);
  json["isActive"] = isActive;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Int64(*updatedAt) : Json::nullValue;
  json["deletedAt"] = deletedAt ? Json::Int64(*deletedAt) : Json::nullValue;
  return json;
}
