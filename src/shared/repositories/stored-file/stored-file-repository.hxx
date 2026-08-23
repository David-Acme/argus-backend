#pragma once

#include "stored-file-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/stored-file/stored-file-schema.hxx>

class StoredFileRepository
{
public:
  StoredFileRepository() = default;

  drogon::Task<StoredFileSchema>
  create(const StoredFileCreateInput& input) const;
  drogon::Task<std::optional<StoredFileSchema>> findById(int64_t id) const;
  drogon::Task<std::optional<StoredFileSchema>>
  findByObjectKey(const std::string& objectKey) const;
  drogon::Task<bool> remove(int64_t id) const;
};
