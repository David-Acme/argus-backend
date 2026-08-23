#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <string>
#include <utility>

class FaceDB
{
public:
  explicit FaceDB(VecDb& vecDb) : vecDb_(vecDb) {}

  void init();
  void shutdown();
  bool insert(const float* embedding, int64_t personId,
              int64_t faceEmbeddingId);
  std::optional<std::pair<int64_t, float>> search(const float* query);
  void remove(int64_t personId);
  size_t count();

private:
  std::mutex& vecMutex();

  VecDb& vecDb_;
  FaceEmbeddingRepository repository_;
};
