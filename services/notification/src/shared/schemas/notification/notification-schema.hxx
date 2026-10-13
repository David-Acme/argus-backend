#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct NotificationSchema
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
  std::optional<int64_t> deletedAt;

  NotificationSchema() = default;
  explicit NotificationSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
