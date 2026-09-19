#include "stored-file-schema.hxx"

#include <drogon/orm/Field.h>

StoredFileSchema::StoredFileSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  objectKey = row["object_key"].as<std::string>();
  sha256 = row["sha256"].as<std::string>();
  mimeType = row["mime_type"].as<std::string>();
  byteSize = static_cast<int64_t>(row["byte_size"].as<long long>());
  category = storedFileCategoryFromString(row["category"].as<std::string>());
  if (!row["created_by"].isNull())
    createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}
