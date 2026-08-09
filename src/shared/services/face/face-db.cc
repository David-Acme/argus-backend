#include "face-db.hxx"

#include <algorithm>
#include <cmath>
#include <drogon/drogon.h>
#include <map>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <string>

namespace
{

constexpr int kEmbeddingDim = 128;
constexpr float kMinConfidence = 0.80F;

std::string encodeVector(const float* data, int count)
{
  std::string out = "[";
  for (int i = 0; i < count; ++i) {
    if (i > 0)
      out += ",";
    out += std::to_string(data[i]);
  }
  out += "]";
  return out;
}

void bindFloatVector(sqlite3_stmt* stmt, int index, const std::string& enc)
{
  sqlite3_bind_text(stmt, index, enc.data(), -1, SQLITE_TRANSIENT);
}

} // namespace

std::mutex& FaceDB::vecMutex()
{
  return VecDb::mutex();
}

void FaceDB::init()
{
  LOG_INFO << "FaceDB: vec0 index ready";
}

void FaceDB::shutdown() {}

void FaceDB::insert(const float* embedding, int64_t personId,
                    int64_t faceEmbeddingId)
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO face_vec (rowid, embedding, person_id, "
          "face_embedding_id) VALUES (?, ?, ?, ?)",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "FaceDB::insert prepare failed: " << sqlite3_errmsg(db);
    return;
  }
  sqlite3_bind_int64(stmt, 1, faceEmbeddingId);
  bindFloatVector(stmt, 2, encodeVector(embedding, kEmbeddingDim));
  sqlite3_bind_int64(stmt, 3, personId);
  sqlite3_bind_int64(stmt, 4, faceEmbeddingId);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::optional<std::pair<int64_t, float>> FaceDB::search(const float* query)
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return std::nullopt;

  const int topK = std::max(1, ConfigService::getInt("face.top_k"));
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT person_id, distance FROM face_vec "
          "WHERE embedding MATCH ? ORDER BY distance LIMIT ?",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "FaceDB::search prepare failed: " << sqlite3_errmsg(db);
    return std::nullopt;
  }
  bindFloatVector(stmt, 1, encodeVector(query, kEmbeddingDim));
  sqlite3_bind_int(stmt, 2, topK);

  std::map<int64_t, float> bestByPerson;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const int64_t personId = sqlite3_column_int64(stmt, 0);
    const float confidence =
        1.0F - static_cast<float>(sqlite3_column_double(stmt, 1));
    if (!std::isfinite(confidence))
      continue;
    auto it = bestByPerson.find(personId);
    if (it == bestByPerson.end() || confidence > it->second)
      bestByPerson[personId] = confidence;
  }
  sqlite3_finalize(stmt);

  if (bestByPerson.empty())
    return std::nullopt;

  auto winner = std::max_element(
      bestByPerson.begin(), bestByPerson.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });

  if (winner->second < kMinConfidence)
    return std::nullopt;

  return std::make_pair(winner->first, winner->second);
}

void FaceDB::remove(int64_t personId)
{
  auto client = DbService::client();
  auto rows = client->execSqlSync(
      "SELECT id FROM face_embedding WHERE person_id = ?", personId);

  std::scoped_lock lock(vecMutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM face_vec WHERE rowid = ?", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "FaceDB::remove prepare failed: " << sqlite3_errmsg(db);
    return;
  }
  for (const auto& row : rows) {
    sqlite3_bind_int64(stmt, 1, row["id"].as<int64_t>());
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
}

size_t FaceDB::count()
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return 0;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM face_vec", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    LOG_WARN << "FaceDB::count prepare failed: " << sqlite3_errmsg(db);
    return 0;
  }
  size_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);
  return count;
}
