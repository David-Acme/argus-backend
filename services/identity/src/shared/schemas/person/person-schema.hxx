#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <identity/person-category.hxx>
#include <shared/vocabulary/person-status.hxx>
#include <string>

struct PersonSchema
{
  int64_t id{0};
  std::optional<int64_t> userId;
  std::string name;
  std::string alias;
  std::string observation;
  PersonStatus status{PersonStatus::Known};
  PersonCategory category{PersonCategory::None};
  std::string note;
  std::optional<int64_t> visitorNumber;
  int64_t visitCount{0};
  int64_t firstSeenAt{0};
  int64_t lastSeenAt{0};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  PersonSchema() = default;
  explicit PersonSchema(const drogon::orm::Row& row);
  [[nodiscard]] Json::Value toJson() const;
  [[nodiscard]] bool isHousehold() const { return userId.has_value(); }
  [[nodiscard]] bool isNamed() const { return !name.empty(); }
};
