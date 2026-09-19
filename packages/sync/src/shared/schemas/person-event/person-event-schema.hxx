#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <string>

struct PersonEventSchema
{
  int64_t personId{0};
  int64_t eventId{0};
  double confidence{0.0};

  PersonEventSchema() = default;
  explicit PersonEventSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
