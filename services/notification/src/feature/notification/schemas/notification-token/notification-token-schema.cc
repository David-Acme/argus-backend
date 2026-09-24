#include "notification-token-schema.hxx"

#include <optional>

NotificationTokenSchema::NotificationTokenSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  deviceHash = row["device_hash"].as<std::string>();
  token = row["token"].as<std::string>();
  platform = row["platform"].as<std::string>();
  lang = row["lang"].as<std::string>();
  isActive = row["is_active"].as<int>() != 0;
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
}

Json::Value NotificationTokenSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["userId"] = userId;
  json["deviceHash"] = deviceHash;
  json["token"] = token;
  json["platform"] = platform;
  json["lang"] = lang;
  json["isActive"] = isActive;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt))
                                : Json::Value();
  return json;
}
