#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct FaceEmbeddingSchema
{
  int64_t id{0};
  int64_t personId{0};
  std::string embedding;
  std::string angleLabel;
  double quality{1.0};
  int64_t createdAt{0};

  FaceEmbeddingSchema() = default;
  explicit FaceEmbeddingSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
