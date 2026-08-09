#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

class FaceDB
{
public:
  FaceDB() = delete;
  ~FaceDB() = delete;

  static void init();
  static void shutdown();
  static void insert(const float* embedding, int64_t personId,
                     int64_t faceEmbeddingId);
  static std::optional<std::pair<int64_t, float>> search(const float* query);
  static void remove(int64_t personId);
  static size_t count();

private:
  static std::mutex& vecMutex();
};
