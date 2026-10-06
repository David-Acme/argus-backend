#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>
#include <auth/user-role.hxx>

struct UserSchema
{
  int64_t id{0};
  std::string name;
  std::string lastName;
  UserRole role{UserRole::Unknown};
  std::string lang{"es"};
  bool isActive{true};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  UserSchema() = default;
  explicit UserSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
