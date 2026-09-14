#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct PersonSchema
{
  int64_t id{0};
  std::optional<int64_t> userId;
  std::string name;
  std::string alias;
  std::string observation;
  PersonStatus status{PersonStatus::Known};
  int64_t firstSeenAt{0};
  int64_t lastSeenAt{0};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  PersonSchema() = default;
  explicit PersonSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
