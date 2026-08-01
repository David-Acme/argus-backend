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
  float temperature{-1.0f};
  // True clears the KV cache before generating (isolated analysis calls).
  bool resetContext{true};
};

using TokenCallback = std::function<void(const std::string& token, bool done)>;

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

private:
  static void warmup();
  static std::string buildPrompt(const std::vector<ChatMessage>& messages);
  static std::string generate(const std::string& formattedPrompt,
                              float temperature, int32_t maxTokens,
                              bool resetContext);
  static void generateStream(const std::string& formattedPrompt,
                             float temperature, int32_t maxTokens,
                             bool resetContext, TokenCallback onToken);

  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static int64_t contextSize_;
  static int32_t defaultMaxTokens_;
  static float defaultTemperature_;
  static bool loaded_;
  static std::mutex mutex_;

  static constexpr const char* SYSTEM_PROMPT =
      R"SYSPROMPT(You are Argus, an intelligent security assistant for a local surveillance system. Your tasks are:
1. Analyze security events (motion, person, vehicle, or anomalous sound detections).
2. Answer questions about the system state: active cameras, recent events, recognized people.
3. Help configure security rules: exclusion zones, watch schedules, detection sensitivity.
4. Explain security alerts in clear, actionable language.
5. You have NO cloud access - all processing is local.

Be concise and natural: max 2-3 lines for simple questions, and only expand a little when the question requires it. Be direct, precise, and prioritize safety. Always answer in the same language the user speaks.)SYSPROMPT";
};
