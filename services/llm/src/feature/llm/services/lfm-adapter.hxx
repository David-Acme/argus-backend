#pragma once

#include <cstdint>
#include <shared/vocabulary/tool-contracts.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <llm/llm-service.hxx>
#include <feature/llm/services/tools/tool-access.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <functional>
#include <string>
#include <vector>

struct ToolChatInput
{
  std::string systemPrompt;
  std::vector<tools::ToolHandle> tools;
  ToolAudience audience;
  tools::ToolContext context;
  std::string clock{};
  int maxHops = 3;
  float temperature = -1.0F;
  float toolTemperature = 0.0F;
  bool resetContext = false;
  int32_t answerMaxTokens = 0;
  bool prefillOnly = false;
};

struct ToolChatOutput
{
  std::string reply;
  std::vector<tools::ToolCall> executed;
  int hops = 0;
  bool emitted = false;
  int64_t generateMs = 0;
  int64_t toolMs = 0;
};

struct TurnState
{
  bool asked{false};
  bool appAsked{false};
  bool wrote{false};
  bool failed{false};
};

struct ToolHopContext
{
  const ToolChatInput& input;
  std::vector<ChatMessage>& history;
  ToolChatOutput& output;
  const TokenCallback* onToken = nullptr;
  const std::string& declarations;
  TurnState& state;
};

struct ChatWithToolsStreamInput
{
  const ToolChatInput& input;
  std::vector<ChatMessage>& history;
  const TokenCallback& onToken;
};

struct StreamHopInput
{
  const ChatRequest& request;
  const TokenCallback& onToken;
  bool& streamed;
  bool holdAll{false};
};

struct ChatEngine
{
  std::function<std::string(const ChatRequest&)> chat;
  std::function<void(const ChatRequest&, TokenCallback)> chatStream;
};

struct LfmAdapterInput
{
  ChatEngine engine;
  ToolRegistry& registry;
  const IntentRouter* router{nullptr};
};

class LfmAdapter
{
public:
  explicit LfmAdapter(LlmService& llm, const IntentRouter* router = nullptr);

  explicit LfmAdapter(LfmAdapterInput input);

  static std::string buildToolDeclarations(const std::vector<tools::ToolHandle>& tools);
  static std::vector<tools::ToolCall> parseToolCalls(const std::string& text);

  static bool mayOpenToolCall(const std::string& text);

  static std::string spokenText(const std::string& content);

  static std::string lastUtterance(const std::vector<ChatMessage>& history);

  static std::string renderToolCall(const tools::ToolCall& call);

  ToolChatOutput chatWithTools(const ToolChatInput& input,
                               std::vector<ChatMessage>& history);

  ToolChatOutput chatWithToolsStream(const ChatWithToolsStreamInput& args);

  ToolExecutor& executor() { return executor_; }

private:
  bool routedTurn(ToolHopContext ctx);

  bool toolHops(ToolHopContext ctx);

  [[nodiscard]] ChatRequest hopRequest(const ToolHopContext& ctx) const;

  void proseAnswer(ToolHopContext ctx, float temperature);

  std::string streamHop(const StreamHopInput& args);

  [[nodiscard]] bool offered(const tools::ToolCall& call, const std::vector<tools::ToolHandle>& tools) const;

  ChatEngine engine_;
  ToolRegistry& registry_;
  const IntentRouter* router_ = nullptr;
  ToolExecutor executor_;
};
