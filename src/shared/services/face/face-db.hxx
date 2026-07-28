#pragma once

#include <cstdint>
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
  static void insert(const float* embedding, int64_t personId);
  static std::optional<std::pair<int64_t, float>> search(const float* query);
  static void remove(int64_t personId);
  static void loadFromDb();
  static size_t count();

private:
  static bool loaded_;
};
