#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <string>
#include <sync/user-action.hxx>

struct UserActionLogSchema
{
  int64_t id{0};
  int64_t userId{0};
  int64_t recordId{0};
  std::string tableName;
  std::string module;
  UserAction action{UserAction::Create};
  Json::Value oldData;
  Json::Value newData;
  std::string ipAddress;
  int64_t createdAt{0};

  UserActionLogSchema() = default;
  explicit UserActionLogSchema(const drogon::orm::Row& row);
  [[nodiscard]] Json::Value toJson() const;
  [[nodiscard]] Json::Value toActivityJson() const;
};
