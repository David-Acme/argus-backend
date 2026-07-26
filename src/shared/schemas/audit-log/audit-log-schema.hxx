#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <string>

struct AuditLogSchema
{
  int64_t id{0};
  int64_t recordId{0};
  std::string tableName;
  int64_t eventTimestamp{0};
  std::string oldData;
  std::string newData;
  int64_t createdAt{0};

  AuditLogSchema() = default;
  explicit AuditLogSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
