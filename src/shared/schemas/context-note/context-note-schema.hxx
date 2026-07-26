#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct ContextNoteSchema
{
  int64_t id{0};
  std::optional<int64_t> createdBy;
  std::string title;
  std::string content;
  std::string tags;
  std::optional<int64_t> validFrom;
  std::optional<int64_t> validUntil;
  bool isActive{true};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ContextNoteSchema() = default;
  explicit ContextNoteSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
