#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct EventSchema
{
  int64_t id{0};
  std::string eventType;
  EventSeverity severity{EventSeverity::Info};
  std::string source;
  std::string summary;
  std::string details;
  int64_t occurredAt{0};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  EventSchema() = default;
  explicit EventSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
