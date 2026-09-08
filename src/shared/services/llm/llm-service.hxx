#pragma once

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_batch;

struct ChatMessage
{
  std::string role;
  std::string content;
};

// Inference request; maxTokens 0 and temperature < 0 keep the configured defaults.
struct ChatRequest
{
  std::vector<ChatMessage> messages;
  int32_t maxTokens{0};
  float temperature{-1.0F};
  bool resetContext{false};
};

using TokenCallback = std::function<void(const std::string& token, bool done)>;

struct LlmPrefillStats
{
  int32_t promptTokens{0};
  int32_t reusedTokens{0};
  int32_t decodedTokens{0};
};

class LlmService
{
public:
  LlmService();
  ~LlmService();

  LlmService(const LlmService&) = delete;
  LlmService& operator=(const LlmService&) = delete;

  static LlmService& instance();

  void init();
  void shutdown();

  std::string chat(const ChatRequest& req);
  void chatStream(const ChatRequest& req, TokenCallback onToken);

  // Coroutine variants: run inference off the event loop.
  drogon::Task<std::string> chatAsync(const ChatRequest& req);
  drogon::Task<void> chatStreamAsync(const ChatRequest& req,
                                     TokenCallback onToken);

  bool isLoaded();
  bool isBusy();
  LlmPrefillStats lastPrefillStats();

  // Read-only config views for the internal wire's config leg.
  int32_t defaultMaxTokens() const { return defaultMaxTokens_; }
  float defaultTemperature() const { return defaultTemperature_; }
  int64_t contextSize() const { return contextSize_; }

  std::string buildPrompt(const std::vector<ChatMessage>& messages);

private:
  void warmup();
  static std::string
  buildChatMlPrompt(const std::vector<ChatMessage>& messages);
  std::vector<int32_t> tokenize(const std::string& text, bool addSpecial);
  bool prefill(const std::vector<int32_t>& promptTokens, bool forceReset);
  std::string generate(const std::string& formattedPrompt, float temperature,
                       int32_t maxTokens, bool resetContext);
  void generateStream(const std::string& formattedPrompt, float temperature,
                      int32_t maxTokens, bool resetContext,
                      TokenCallback onToken);

  std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  std::unique_ptr<llama_batch> promptBatch_;
  std::unique_ptr<llama_batch> genBatch_;
  std::vector<int32_t> cachedTokens_;
  std::string chatTemplate_;
  int64_t contextSize_ = 0;
  int32_t nBatch_ = 1024;
  int32_t defaultMaxTokens_ = 96;
  float defaultTemperature_ = 0.3F;
  int32_t topK_ = 20;
  float topP_ = 0.8F;
  int32_t penaltyLastN_ = 64;
  float penaltyRepeat_ = 1.1F;
  float penaltyFreq_ = 0.0F;
  float penaltyPresent_ = 0.0F;
  uint32_t seed_ = 0;
  LlmPrefillStats lastStats_;
  bool loaded_ = false;
  std::mutex mutex_;
  std::atomic<bool> busy_{false};
};
