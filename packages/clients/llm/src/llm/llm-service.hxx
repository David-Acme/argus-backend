#pragma once

#include <atomic>
#include <auth/user-role.hxx>
#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
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
  int32_t maxTokens{0};
  float temperature{-1.0F};
  bool resetContext{false};
  bool toolsEnabled{true};
  std::vector<std::string> stop;
  std::string grammar;
  bool grammarRequired{false};
  int64_t userId{0};
  UserRole role{UserRole::Guest};
  std::string lang{};
  bool clientActions{false};
  std::string sessionId{};
  bool toolCallsAllowed{true};
  bool prefillOnly{false};
};

using TokenCallback = std::function<void(const std::string& token, bool done)>;

struct ClientAction
{
  std::string name;
  std::string arguments;
};

using ActionCallback = std::function<void(const ClientAction& action)>;

struct LlmPrefillStats
{
  int32_t promptTokens{0};
  int32_t reusedTokens{0};
  int32_t decodedTokens{0};
};

struct LlmStreamInput
{
  ChatRequest request;
  TokenCallback onToken;
  LlmPrefillStats* stats{nullptr};
  std::stop_token cancellation{};
  ActionCallback onAction{};
};

struct GenerateInput
{
  std::string formattedPrompt;
  float temperature{-1.0F};
  int32_t maxTokens{0};
  bool resetContext{false};
  std::vector<std::string> stop;
  std::string grammar;
  bool grammarRequired{false};
  bool toolCallsAllowed{true};
  bool prefillOnly{false};
};

struct LlmSampling
{
  int32_t maxTokens{256};
  float temperature{0.85F};
  int32_t topK{20};
  float topP{0.8F};
  float minP{0.0F};
  int32_t penaltyLastN{64};
  float penaltyRepeat{1.1F};
  float penaltyFreq{0.0F};
  float penaltyPresent{0.0F};
  uint32_t seed{std::numeric_limits<uint32_t>::max()};
};

struct PrefixCheckpoint
{
  std::size_t tokens{0};
  std::vector<std::uint8_t> state;
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

  drogon::Task<std::string> chatAsync(const ChatRequest& req);
  drogon::Task<void> chatStreamAsync(const ChatRequest& req,
                                     TokenCallback onToken);

  bool isLoaded();
  bool isBusy();
  LlmPrefillStats lastPrefillStats();

  void refreshSampling();
  [[nodiscard]] LlmSampling sampling() const;
  int32_t defaultMaxTokens() const { return sampling().maxTokens; }
  float defaultTemperature() const { return sampling().temperature; }
  int64_t contextSize() const { return contextSize_; }

  std::string buildPrompt(const std::vector<ChatMessage>& messages);

private:
  void warmup();
  static std::string
  buildChatMlPrompt(const std::vector<ChatMessage>& messages);
  std::vector<int32_t> tokenize(const std::string& text, bool addSpecial);
  int32_t specialToken(const std::string& text);
  bool prefill(const std::vector<int32_t>& promptTokens, bool forceReset);
  std::size_t rewind(std::size_t target);
  void checkpoint(std::size_t tokens);
  void forgetCache();
  GenerateInput generateInput(const ChatRequest& req);
  std::string generate(const GenerateInput& input);
  void generateStream(const GenerateInput& input, TokenCallback onToken);

  std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  std::unique_ptr<llama_batch> promptBatch_;
  std::unique_ptr<llama_batch> genBatch_;
  std::vector<int32_t> cachedTokens_;
  std::vector<PrefixCheckpoint> checkpoints_;
  bool tailLocked_ = false;
  int32_t messageStart_ = -1;
  int32_t toolCallStart_ = -1;
  std::vector<int32_t> endTokens_;
  std::string chatTemplate_;
  int64_t contextSize_ = 0;
  int32_t nBatch_ = 1024;
  mutable std::mutex samplingMutex_;
  LlmSampling sampling_;
  std::atomic<LlmPrefillStats> lastStats_;
  bool loaded_ = false;
  std::mutex mutex_;
  std::atomic<bool> busy_{false};
};
