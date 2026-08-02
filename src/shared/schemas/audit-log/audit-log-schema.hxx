#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct AuditLogSchema
{
  int64_t id{0};
  std::optional<int64_t> createUserId;
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  int64_t eventTimestamp{0};
  int64_t createdAt{0};

  AuditLogSchema() = default;
  explicit AuditLogSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
