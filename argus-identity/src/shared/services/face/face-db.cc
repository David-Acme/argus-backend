#include "face-db.hxx"

#include <algorithm>
#include <cmath>
#include <drogon/drogon.h>
#include <map>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <sqlite3.h>

namespace
{

constexpr int kEmbeddingDim = 128;
constexpr float kMinConfidence = 0.80F;

} // namespace

std::mutex& FaceDB::vecMutex()
{
  return vecDb_.mutex();
}

void FaceDB::init()
{
  LOG_INFO << "FaceDB: vec0 index ready";
}

void FaceDB::shutdown() {}

bool FaceDB::insert(const float* embedding, int64_t personId,
                    int64_t faceEmbeddingId)
{
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

  auto winner = std::max_element(bestByPerson.begin(), bestByPerson.end(),
                                 [](const auto& a, const auto& b) {
                                   return a.second < b.second;
                                 });

  if (winner->second < kMinConfidence)
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

size_t FaceDB::count()
{
  std::scoped_lock lock(vecMutex());
  sqlite3* db = vecDb_.handle();
  if (!db)
    return 0;
  return repository_.countVec(db);
}
