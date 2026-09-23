#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <llm/tool-contracts.hxx>
#include <shared/services/intent/intent-router.hxx>
#include <llm/llm-service.hxx>
#include <shared/services/tools/tool-executor.hxx>
#include <string>
#include <vector>

struct ToolChatInput
{
  std::string systemPrompt;
  std::vector<const tools::ToolDescriptor*> tools;
  UserRole role;
  tools::ToolContext context;
  int maxHops = 3;
  float temperature = -1.0F;
  float toolTemperature = 0.0F;
  bool resetContext = false;
  int32_t answerMaxTokens = 0;
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

struct ToolHopContext
{
  const ToolChatInput& input;
  std::vector<ChatMessage>& history;
  ToolChatOutput& output;
  const TokenCallback* onToken = nullptr;
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
};

class LfmAdapter
{
public:
  explicit LfmAdapter(LlmService& llm, const IntentRouter* router = nullptr)
      : llm_(llm), router_(router), executor_(ToolRegistry::instance())
  {
  }

  static std::string
  buildToolDeclarations(const std::vector<const tools::ToolDescriptor*>& tools);
  static std::vector<tools::ToolCall> parseToolCalls(const std::string& text);

  static bool mayOpenToolCall(const std::string& text);

  ToolChatOutput chatWithTools(const ToolChatInput& input,
                               std::vector<ChatMessage>& history);

  ToolChatOutput chatWithToolsStream(const ChatWithToolsStreamInput& args);

private:
  bool routedTurn(ToolHopContext ctx);

  bool toolHops(ToolHopContext ctx, const std::string& declarations);

  void proseAnswer(ToolHopContext ctx, float temperature);

  std::string streamHop(const StreamHopInput& args);

  LlmService& llm_;
  const IntentRouter* router_ = nullptr;
  ToolExecutor executor_;
};
