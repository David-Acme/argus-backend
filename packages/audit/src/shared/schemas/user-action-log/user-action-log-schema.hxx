#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <shared/enums.hxx>
#include <string>

struct UserActionLogSchema
{
  int64_t id{0};
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  UserAction action{UserAction::Create};
  Json::Value oldData;
  Json::Value newData;
  std::string ipAddress;
  int64_t createdAt{0};

  UserActionLogSchema() = default;
  explicit UserActionLogSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
