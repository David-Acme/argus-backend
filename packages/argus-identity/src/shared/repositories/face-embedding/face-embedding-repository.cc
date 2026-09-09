#include "face-embedding-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>

using namespace face_embedding_query;

namespace
{

bool bindFloatVector(SqliteStmt& stmt, int index, const float* data, int count)
{
  return stmt.bindBlob({.index = index,
                        .data = data,
                        .size = static_cast<size_t>(count) * sizeof(float)});
}

} // namespace

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

std::vector<int64_t>
FaceEmbeddingRepository::findIdsByPerson(sqlite3* db, int64_t personId) const
{
  std::vector<int64_t> ids;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_IDS_BY_PERSON.data()))
    return ids;
  stmt.bindInt64(1, personId);
  while (stmt.step() == SQLITE_ROW)
    ids.push_back(stmt.columnInt64(0));
  return ids;
}

bool FaceEmbeddingRepository::insertVec(sqlite3* db,
                                        const FaceVecInsertInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_INSERT.data())) {
    LOG_ERROR << "FaceDB: vector insert preparation failed: "
              << sqlite3_errmsg(db);
    return false;
  }

  if (!stmt.bindInt64(1, input.faceEmbeddingId) ||
      !bindFloatVector(stmt, 2, input.embedding, input.dims) ||
      !stmt.bindInt64(3, input.personId) ||
      !stmt.bindInt64(4, input.faceEmbeddingId)) {
    LOG_ERROR << "FaceDB: vector insert binding failed: " << sqlite3_errmsg(db);
    return false;
  }

  const int rc = stmt.step();
  if (rc != SQLITE_DONE) {
    LOG_ERROR << "FaceDB: vector insert failed (rc=" << rc
              << "): " << sqlite3_errmsg(db);
    return false;
  }
  return true;
}

std::vector<FaceVecHit>
FaceEmbeddingRepository::searchVec(sqlite3* db,
                                   const FaceVecSearchInput& input) const
{
  std::vector<FaceVecHit> hits;
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_SEARCH.data()))
    return hits;
  bindFloatVector(stmt, 1, input.query, input.dims);
  stmt.bindInt(2, input.topK);
  while (stmt.step() == SQLITE_ROW) {
    hits.push_back({.personId = stmt.columnInt64(0),
                    .distance = static_cast<float>(stmt.columnDouble(1))});
  }
  return hits;
}

bool FaceEmbeddingRepository::deleteVecRow(sqlite3* db, int64_t rowid) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_DELETE.data()))
    return false;
  stmt.bindInt64(1, rowid);
  return stmt.step() == SQLITE_DONE;
}

size_t FaceEmbeddingRepository::countVec(sqlite3* db) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_COUNT.data()))
    return 0;
  if (stmt.step() != SQLITE_ROW)
    return 0;
  return static_cast<size_t>(stmt.columnInt64(0));
}
