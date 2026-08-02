#include "notification-schema.hxx"

#include <shared/utils/json-util/json-util.hxx>

NotificationSchema::NotificationSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  type = row["type"].as<std::string>();
  title = row["title"].as<std::string>();
  body = row["body"].as<std::string>();
  data = json_util::fromString(row["data"].as<std::string>());
  isRead = row["is_read"].as<int>() != 0;
  if (!row["read_at"].isNull())
    readAt = static_cast<int64_t>(row["read_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}

Json::Value NotificationSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["userId"] = userId;
  json["type"] = type;
  json["title"] = title;
  json["body"] = body;
  json["data"] = data;
  json["isRead"] = isRead;
  json["readAt"] = readAt ? Json::Value(Json::Int64(*readAt)) : Json::Value();
  json["createdAt"] = Json::Int64(createdAt);
  return json;
}
