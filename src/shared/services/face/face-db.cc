#include "face-db.hxx"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <drogon/drogon.h>
#include <hnswlib/hnswlib/hnswlib.h>
#include <map>
#include <memory>
#include <mutex>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int kEmbeddingDim = 128;
constexpr int kHnswM = 16;
constexpr int kHnswEfConstruction = 200;
constexpr float kMinConfidence = 0.80F;

std::unique_ptr<hnswlib::InnerProductSpace> g_space;
std::unique_ptr<hnswlib::HierarchicalNSW<float>> g_index;
std::mutex g_mutex;

// hnswlib updates a point IN PLACE when its label already exists, so
// personId can never be used as a label.
size_t g_nextLabel = 0;
size_t g_liveCount = 0;
std::unordered_map<size_t, int64_t> g_labelToPerson;
std::unordered_map<int64_t, std::vector<size_t>> g_personLabels;

} // namespace

bool FaceDB::loaded_ = false;

void FaceDB::init()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_space = std::make_unique<hnswlib::InnerProductSpace>(kEmbeddingDim);
  g_index =
      std::make_unique<hnswlib::HierarchicalNSW<float>>(g_space.get(), 100000,
                                                        kHnswM,
                                                        kHnswEfConstruction);
  g_index->setEf(ConfigService::getInt("face.ef_search"));
  g_nextLabel = 0;
  g_liveCount = 0;
  g_labelToPerson.clear();
  g_personLabels.clear();
  loaded_ = true;
  LOG_INFO << "FaceDB: HNSW index initialized (dim=" << kEmbeddingDim
           << ", M=" << kHnswM
           << ", ef=" << ConfigService::getInt("face.ef_search") << ")";
}

void FaceDB::shutdown()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_index.reset();
  g_space.reset();
  g_labelToPerson.clear();
  g_personLabels.clear();
  loaded_ = false;
  LOG_INFO << "FaceDB shutdown";
}

void FaceDB::insert(const float* embedding, int64_t personId)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index)
    return;

  const size_t label = ++g_nextLabel;
  g_index->addPoint(embedding, label);
  g_labelToPerson[label] = personId;
  g_personLabels[personId].push_back(label);
  ++g_liveCount;
}

std::optional<std::pair<int64_t, float>> FaceDB::search(const float* query)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index) {
    LOG_WARN << "FaceDB::search: no index";
    return std::nullopt;
  }

  const int topK = std::max(1, ConfigService::getInt("face.top_k"));
  auto pq = g_index->searchKnn(query, topK);
  if (pq.empty()) {
    LOG_WARN << "FaceDB::search: empty result";
    return std::nullopt;
  }

  // Best confidence per person (multiple embeddings per person may match).
  std::map<int64_t, float> bestByPerson;
  while (!pq.empty()) {
    auto top = pq.top();
    pq.pop();
    auto it = g_labelToPerson.find(static_cast<size_t>(top.second));
    if (it == g_labelToPerson.end())
      continue;
    const float confidence = 1.0F - top.first;
    if (!std::isfinite(confidence))
      continue;
    auto bIt = bestByPerson.find(it->second);
    if (bIt == bestByPerson.end() || confidence > bIt->second)
      bestByPerson[it->second] = confidence;
  }
  if (bestByPerson.empty()) {
    LOG_WARN << "FaceDB::search: no known labels in result";
    return std::nullopt;
  }

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
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index)
    return;

  auto it = g_personLabels.find(personId);
  if (it == g_personLabels.end())
    return;

  for (size_t label : it->second) {
    g_index->markDelete(label);
    g_labelToPerson.erase(label);
  }
  g_liveCount -= it->second.size();
  g_personLabels.erase(it);
}

size_t FaceDB::count()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_liveCount;
}

void FaceDB::loadFromDb()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_space)
    return;

  auto client = DbService::client();
  auto result =
      client->execSqlSync("SELECT person_id, embedding FROM face_embedding");

  int loaded = 0;
  for (const auto& row : result) {
    int64_t personId = row["person_id"].as<int64_t>();
    std::string hexEmb = row["embedding"].as<std::string>();

    int numFloats = static_cast<int>(hexEmb.size() / 8);
    if (numFloats != kEmbeddingDim)
      continue;

    std::vector<float> emb;
    emb.reserve(numFloats);
    for (int i = 0; i < numFloats; ++i) {
      uint32_t val = 0;
      for (int j = 0; j < 8; ++j) {
        char c = hexEmb[i * 8 + j];
        val <<= 4;
        if (c >= '0' && c <= '9')
          val |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
          val |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
          val |= static_cast<uint32_t>(c - 'A' + 10);
      }
      float f;
      std::memcpy(&f, &val, sizeof(f));
      emb.push_back(f);
    }

    const size_t label = ++g_nextLabel;
    g_index->addPoint(emb.data(), label);
    g_labelToPerson[label] = personId;
    g_personLabels[personId].push_back(label);
    ++g_liveCount;
    ++loaded;
  }

  LOG_INFO << "FaceDB: loaded " << loaded << " embeddings from storage";
}
