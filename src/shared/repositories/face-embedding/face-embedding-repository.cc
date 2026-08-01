#include "face-embedding-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace face_embedding_query;

drogon::Task<std::optional<FaceEmbeddingSchema>>
FaceEmbeddingRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return FaceEmbeddingSchema(result.front());
}

drogon::Task<std::vector<FaceEmbeddingSchema>>
FaceEmbeddingRepository::findByPerson(int64_t personId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_PERSON.data(), personId);

  std::vector<FaceEmbeddingSchema> embeddings;
  embeddings.reserve(result.size());
  for (const auto& row : result) {
    embeddings.emplace_back(row);
  }
  co_return embeddings;
}

drogon::Task<FaceEmbeddingSchema>
FaceEmbeddingRepository::create(const FaceEmbeddingCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.personId,
                                   input.embedding, input.angleLabel,
                                   input.quality);

  FaceEmbeddingSchema schema;
  schema.id = result.insertId();
  schema.personId = input.personId;
  schema.embedding = input.embedding;
  schema.angleLabel = input.angleLabel;
  schema.quality = input.quality;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<bool>
FaceEmbeddingRepository::removeByPerson(int64_t personId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(DELETE_BY_PERSON.data(), personId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<FaceEmbeddingSchema>>
FaceEmbeddingRepository::findAll() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_ALL.data());

  std::vector<FaceEmbeddingSchema> embeddings;
  embeddings.reserve(result.size());
  for (const auto& row : result) {
    embeddings.emplace_back(row);
  }
  co_return embeddings;
}
