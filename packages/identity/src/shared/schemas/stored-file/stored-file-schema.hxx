#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct StoredFileSchema
{
  int64_t id{0};
  std::string objectKey;
  std::string sha256;
  std::string mimeType;
  int64_t byteSize{0};
  StoredFileCategory category{StoredFileCategory::Portrait};
  std::optional<int64_t> createdBy;
  int64_t createdAt{0};
  std::optional<int64_t> deletedAt;

  StoredFileSchema() = default;
  explicit StoredFileSchema(const drogon::orm::Row& row);
};
