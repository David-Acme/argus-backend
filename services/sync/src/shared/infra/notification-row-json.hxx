#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

struct NotificationRowJson
{
  int64_t id{0};
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
  bool isRead{false};
  std::optional<int64_t> readAt;
  int64_t createdAt{0};

  [[nodiscard]] Json::Value toJson() const
  {
    Json::Value json(Json::objectValue);
    json["id"] = Json::Int64(id);
    json["userId"] = Json::Int64(userId);
    json["type"] = type;
    json["title"] = title;
    json["body"] = body;
    json["data"] = data;
    json["isRead"] = isRead;
    json["readAt"] = readAt ? Json::Value(Json::Int64(*readAt)) : Json::Value();
    json["createdAt"] = Json::Int64(createdAt);
    return json;
  }
};
