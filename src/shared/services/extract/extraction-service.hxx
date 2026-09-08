#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_batch;
struct llama_sampler;

enum class ExtractPromptFormat
{
  V15,
  V2,
  Lfm,
};

ExtractPromptFormat extractPromptFormatFromString(const std::string& raw);

struct ExtractRequest
{
  std::string text;
  std::string templateJson;
  std::string schemaJson;
  int maxTokens = 512;
  bool grammar = true;
  CancellationToken cancel;
};

struct PromptInput
{
  std::string templateJson;
  std::string schemaJson;
  std::string text;
  ExtractPromptFormat format;
};

class ExtractionService
{
public:
  ExtractionService();
  ~ExtractionService();

  ExtractionService(const ExtractionService&) = delete;
  ExtractionService& operator=(const ExtractionService&) = delete;

  bool ensureLoaded();
  void unloadIfIdle();
  std::optional<Json::Value> extract(const ExtractRequest& request);
  drogon::Task<std::optional<Json::Value>>
  extractAsync(const ExtractRequest& request);
  bool isLoaded() const { return loaded_; }

  static std::string buildPrompt(const PromptInput& input);
  static std::string buildGrammar(const Json::Value& templateJson,
                                  const std::string& rawOrder);

private:
  struct ContextSlot
  {
    std::unique_ptr<llama_context, void (*)(llama_context*)> ctx{nullptr,
                                                                 &llamaFree};
    std::unique_ptr<llama_batch> batch;
    std::unique_ptr<llama_batch> genBatch;
    std::mutex mtx;

    static void llamaSamplerFree(llama_sampler* s);
  };

  bool loadLocked();
  std::optional<Json::Value> extractOnSlot(ContextSlot& slot,
                                           const ExtractRequest& request);
  std::string grammarFor(const std::string& canonicalTemplate,
                         const Json::Value& parsedTemplate,
                         const std::string& rawOrder);

  std::unique_ptr<llama_model, void (*)(llama_model*)> model_{nullptr,
                                                              &llamaModelFree};
  std::vector<std::unique_ptr<ContextSlot>> slots_;
  std::counting_semaphore<8> permits_{0};
  mutable std::mutex loadMutex_;
  mutable std::mutex grammarMutex_;
  std::string grammarKey_;
  std::string grammarValue_;
  bool loaded_ = false;
  int maxTokens_ = 512;
  int nCtx_ = 2048;
  int threads_ = 1;
  ExtractPromptFormat format_ = ExtractPromptFormat::V15;

  static void llamaModelFree(llama_model* m);
  static void llamaFree(llama_context* c);
};
