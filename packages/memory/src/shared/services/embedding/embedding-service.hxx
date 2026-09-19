#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <shared/services/embedding/unigram-tokenizer.hxx>
#include <string>
#include <vector>

namespace Ort
{
struct Env;
struct Session;
} // namespace Ort

class EmbeddingService
{
public:
  EmbeddingService();
  ~EmbeddingService();

  EmbeddingService(const EmbeddingService&) = delete;
  EmbeddingService& operator=(const EmbeddingService&) = delete;

  void init();
  void shutdown();
  bool isLoaded() const;
  int dimensions() const;

  std::optional<std::vector<float>> embed(const std::string& text,
                                          const std::string& prefix);

private:
  bool ensureLoadedLocked();

  mutable std::mutex mutex_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  UnigramTokenizer tokenizer_;
  int maxLen_ = 512;
  int dim_ = 384;
  int outDim_ = 0;
  bool loaded_ = false;
  bool initAttempted_ = false;
};
