#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct NotificationTokenSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string deviceHash;
  std::string token;
  std::string platform;
  std::string lang;
  bool isActive{true};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;

  NotificationTokenSchema() = default;
  explicit NotificationTokenSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
