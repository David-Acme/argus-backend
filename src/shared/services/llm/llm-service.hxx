#pragma once

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

struct ChatRequest
{
  std::vector<ChatMessage> messages;
  // 0 = use the configured default (llm.max_tokens).
  int32_t maxTokens{0};
  // < 0 = use the configured default (llm.temperature).
  float temperature{-1.0F};
  // Forces a full KV reset. Not needed for isolation: a prompt that diverges
  // from the cached prefix already triggers a reset automatically.
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
  LlmService() = delete;
  ~LlmService() = delete;

  static void init();
  static void shutdown();

  static std::string chat(const ChatRequest& req);
  static void chatStream(const ChatRequest& req, TokenCallback onToken);

  // Coroutine variants: run inference off the event loop.
  static drogon::Task<std::string> chatAsync(const ChatRequest& req);
  static drogon::Task<void> chatStreamAsync(const ChatRequest& req,
                                            TokenCallback onToken);

  static bool isLoaded();
  static LlmPrefillStats lastPrefillStats();

private:
  static void warmup();
  static std::string buildPrompt(const std::vector<ChatMessage>& messages);
  static std::string buildChatMlPrompt(const std::vector<ChatMessage>& messages);
  static std::vector<int32_t> tokenize(const std::string& text, bool addSpecial);
  static bool prefill(const std::vector<int32_t>& promptTokens,
                      bool forceReset);
  static std::string generate(const std::string& formattedPrompt,
                              float temperature, int32_t maxTokens,
                              bool resetContext);
  static void generateStream(const std::string& formattedPrompt,
                             float temperature, int32_t maxTokens,
                             bool resetContext, TokenCallback onToken);

  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static std::unique_ptr<llama_batch> promptBatch_;
  static std::unique_ptr<llama_batch> genBatch_;
  static std::vector<int32_t> cachedTokens_;
  static std::string chatTemplate_;
  static int64_t contextSize_;
  static int32_t nBatch_;
  static int32_t defaultMaxTokens_;
  static float defaultTemperature_;
  static int32_t topK_;
  static float topP_;
  static int32_t penaltyLastN_;
  static float penaltyRepeat_;
  static float penaltyFreq_;
  static float penaltyPresent_;
  static uint32_t seed_;
  static LlmPrefillStats lastStats_;
  static bool loaded_;
  static std::mutex mutex_;
};
