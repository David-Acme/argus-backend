#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct llama_model;
struct llama_context;

struct ChatRequest
{
  std::string prompt;
  int32_t maxTokens{4096};
  float temperature{0.7f};
};

class LlmService
{
public:
  LlmService() = delete;
  ~LlmService() = delete;

  static void init();
  static void shutdown();

  static std::string chat(const ChatRequest& req);

  static bool isLoaded();

private:
  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static int64_t contextSize_;
  static bool loaded_;
};
