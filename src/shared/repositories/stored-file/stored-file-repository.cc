#include "stored-file-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace stored_file_query;

drogon::Task<StoredFileSchema>
StoredFileRepository::create(const StoredFileCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.objectKey, input.sha256, input.mimeType,
      input.byteSize, storedFileCategoryToString(input.category), input.createdBy);

  StoredFileSchema schema;
  schema.id = result.insertId();
  schema.objectKey = input.objectKey;
  schema.sha256 = input.sha256;
  schema.mimeType = input.mimeType;
  schema.byteSize = input.byteSize;
  schema.category = input.category;
  schema.createdBy = input.createdBy;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<StoredFileSchema>>
StoredFileRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return StoredFileSchema(result.front());
}

drogon::Task<std::optional<StoredFileSchema>>
StoredFileRepository::findByObjectKey(const std::string& objectKey) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_OBJECT_KEY.data(), objectKey);
  if (result.empty())
    co_return std::nullopt;
  co_return StoredFileSchema(result.front());
}

drogon::Task<bool> StoredFileRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}
