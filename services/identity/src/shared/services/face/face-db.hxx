#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <span>
#include <sqlite/vec-db.hxx>
#include <string>
#include <utility>
#include <vector>

struct FaceInsertInput
{
  const float* embedding{nullptr};
  int64_t personId{0};
  int64_t faceEmbeddingId{0};
};

struct FaceNeighbour
{
  int64_t personId{0};
  float score{0.0F};
};

struct FaceNearestInput
{
  const float* query{nullptr};
  int topK{0};
};

class FaceDB
{
public:
  explicit FaceDB(VecDb& vecDb) : vecDb_(vecDb) {}

  void init();
  void shutdown();
  bool insert(const FaceInsertInput& input);
  std::optional<std::pair<int64_t, float>> search(const float* query);
  std::vector<FaceNeighbour> nearest(const FaceNearestInput& input);
  void remove(int64_t personId);
  void removeEmbeddings(std::span<const int64_t> faceEmbeddingIds);
  size_t count();
  static float matchThreshold();

private:
  std::mutex& vecMutex();

  VecDb& vecDb_;
  FaceEmbeddingRepository repository_;
};
