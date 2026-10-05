#include "face-db.hxx"

#include <algorithm>
#include <cmath>
#include <drogon/drogon.h>
#include <map>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <config/config-service.hxx>
#include <sqlite/vec-db.hxx>
#include <sqlite3.h>

namespace
{

constexpr int kEmbeddingDim = 128;
constexpr double kDefaultMatchThreshold = 0.50;

}

float FaceDB::matchThreshold()
{
  if (!ConfigService::hasKey("face.match_threshold"))
    return static_cast<float>(kDefaultMatchThreshold);
  return static_cast<float>(std::clamp(
      ConfigService::getDouble("face.match_threshold"), 0.30, 0.95));
}

std::mutex& FaceDB::vecMutex()
{
  return vecDb_.mutex();
}

void FaceDB::init()
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (db) {
    const auto stale = repository_.findStaleVecRows(db, kFaceModelId);
    for (const int64_t rowid : stale)
      repository_.deleteVecRow(db, rowid);
    if (!stale.empty())
      LOG_INFO << "FaceDB: dropped " << stale.size()
               << " index row(s) whose embedding is gone or belongs to "
                  "another face model";
  }
  LOG_INFO << "FaceDB: vec0 index ready";
}

void FaceDB::shutdown() {}

bool FaceDB::insert(const FaceInsertInput& input)
{
  const float* embedding = input.embedding;
  const int64_t personId = input.personId;
  const int64_t faceEmbeddingId = input.faceEmbeddingId;

  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (!db) {
    LOG_ERROR << "FaceDB: vector database is unavailable while enrolling "
              << "person " << personId;
    return false;
  }

  const bool inserted = repository_.insertVec(
      db, {.embedding = embedding,
           .dims = kEmbeddingDim,
           .personId = personId,
           .faceEmbeddingId = faceEmbeddingId});
  if (!inserted) {
    LOG_ERROR << "FaceDB: could not index face embedding " << faceEmbeddingId
              << " for person " << personId;
  }
  return inserted;
}

std::optional<std::pair<int64_t, float>> FaceDB::search(const float* query)
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (!db)
    return std::nullopt;

  const int topK = std::max(1, ConfigService::getInt("face.top_k"));
  const auto hits = repository_.searchVec(db, {.query = query,
                                               .dims = kEmbeddingDim,
                                               .topK = topK});

  std::map<int64_t, float> bestByPerson;
  for (const auto& hit : hits) {
    const float confidence = 1.0F - hit.distance;
    if (!std::isfinite(confidence))
      continue;
    auto it = bestByPerson.find(hit.personId);
    if (it == bestByPerson.end() || confidence > it->second)
      bestByPerson[hit.personId] = confidence;
  }

  if (bestByPerson.empty())
    return std::nullopt;

  const auto winner = std::ranges::max_element(
      bestByPerson, {}, [](const auto& entry) { return entry.second; });

  if (winner->second < matchThreshold())
    return std::nullopt;

  return std::make_pair(winner->first, winner->second);
}

void FaceDB::remove(int64_t personId)
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (!db)
    return;
  for (const int64_t id : repository_.findIdsByPerson(db, personId))
    repository_.deleteVecRow(db, id);
}

void FaceDB::removeEmbeddings(std::span<const int64_t> faceEmbeddingIds)
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (!db)
    return;
  for (const int64_t id : faceEmbeddingIds)
    repository_.deleteVecRow(db, id);
}

size_t FaceDB::count()
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (!db)
    return 0;
  return repository_.countVec(db);
}
