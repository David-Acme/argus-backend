#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

class EmbeddingService
{
public:
  EmbeddingService() = delete;
  ~EmbeddingService() = delete;

  static void init();
  static void shutdown();
  static bool isLoaded();
  static int dimensions();

  static std::optional<std::vector<float>> embed(const std::string& text,
                                                 const std::string& prefix);
};
