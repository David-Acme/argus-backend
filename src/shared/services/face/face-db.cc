#include "face-db.hxx"

#include <drogon/drogon.h>
#include <hnswlib/hnswlib/hnswlib.h>
#include <memory>
#include <mutex>
#include <shared/services/sqlite/db-service.hxx>

namespace
{

constexpr int kEmbeddingDim = 128;
constexpr int kHnswM = 16;
constexpr int kHnswEfConstruction = 200;
constexpr int kHnswEfSearch = 50;

std::unique_ptr<hnswlib::InnerProductSpace> g_space;
std::unique_ptr<hnswlib::HierarchicalNSW<float>> g_index;
std::mutex g_mutex;

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
  g_index->setEf(kHnswEfSearch);
  loaded_ = true;
  LOG_INFO << "FaceDB: HNSW index initialized (dim=" << kEmbeddingDim
           << ", M=" << kHnswM << ")";
}

void FaceDB::shutdown()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  g_index.reset();
  g_space.reset();
  loaded_ = false;
  LOG_INFO << "FaceDB shutdown";
}

void FaceDB::insert(const float* embedding, int64_t personId)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index)
    return;
  g_index->addPoint(embedding, static_cast<size_t>(personId));
  LOG_INFO << "FaceDB::insert person=" << personId
           << " count=" << g_index->getCurrentElementCount();
}

std::optional<std::pair<int64_t, float>> FaceDB::search(const float* query)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index) {
    LOG_WARN << "FaceDB::search: no index";
    return std::nullopt;
  }

  auto pq = g_index->searchKnn(query, 1);
  if (pq.empty()) {
    LOG_WARN << "FaceDB::search: empty result";
    return std::nullopt;
  }

  auto top = pq.top();
  float dist = top.first;
  LOG_INFO << "FaceDB::search: dist=" << dist << " cos=" << (1.0F - dist)
           << " person=" << top.second
           << " count=" << g_index->getCurrentElementCount();

  if (dist > 0.20F)
    return std::nullopt;

  return std::make_pair(static_cast<int64_t>(top.second), 1.0F - dist);
}

void FaceDB::remove(int64_t personId)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index)
    return;
  g_index->markDelete(static_cast<size_t>(personId));
}

size_t FaceDB::count()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index)
    return 0;
  return g_index->getCurrentElementCount();
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

    int numFloats = hexEmb.size() / 8;
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
          val |= (c - '0');
        else if (c >= 'a' && c <= 'f')
          val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
          val |= (c - 'A' + 10);
      }
      float f;
      std::memcpy(&f, &val, sizeof(f));
      emb.push_back(f);
    }

    g_index->addPoint(emb.data(), static_cast<size_t>(personId));
    ++loaded;
  }

  LOG_INFO << "FaceDB: loaded " << loaded << " embeddings from storage";
}
