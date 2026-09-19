#pragma once

#include <cstdint>
#include <shared/contracts/tool-contracts.hxx>
#include <shared/services/intent/intent-router.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/tools/tool-executor.hxx>
#include <string>
#include <vector>

// Tool-loop request; `temperature` -1 keeps the LlmService default ([llm] temperature).
struct ToolChatInput
{
  std::string systemPrompt;
  std::vector<const tools::ToolDescriptor*> tools;
  UserRole role;
  tools::ToolContext context;
  int maxHops = 3;
  float temperature = -1.0F;
  // Tool-bearing hops run cold (measured in the f8-b1 gate).
  float toolTemperature = 0.0F;
  // Hop one honors the caller's session reset; later hops continue.
  bool resetContext = false;
  // Token cap for every generation; 0 keeps 512.
  int32_t answerMaxTokens = 0;
};

struct ToolChatOutput
{
  std::string reply;
  std::vector<tools::ToolCall> executed;
  int hops = 0;
  // The stream leg already put `reply` on the wire token by token.
  bool emitted = false;
  // Model generation against tool execution, in milliseconds.
  int64_t generateMs = 0;
  int64_t toolMs = 0;
};

// One turn's mutable state, threaded through the loop's stages.
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

// Tool-calling adapter over LlmService; parses raw JSON, pythonic and marker-wrapped calls.
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

  // True while `text` can still be the opening of a tool call.
  static bool mayOpenToolCall(const std::string& text);

  ToolChatOutput chatWithTools(const ToolChatInput& input,
                               std::vector<ChatMessage>& history);

  // Same loop, streaming every hop.
  ToolChatOutput chatWithToolsStream(const ChatWithToolsStreamInput& args);

private:
  // The router's own path: its tool runs and one prose answer closes the turn.
  bool routedTurn(ToolHopContext ctx);

  // Hops with tool declarations until the model answers in prose.
  bool toolHops(ToolHopContext ctx, const std::string& declarations);

  // The turn's closing generation; the routed path runs it cold.
  void proseAnswer(ToolHopContext ctx, float temperature);

  // Streams a hop, holding tokens back while a call may still open.
  std::string streamHop(const StreamHopInput& args);

  LlmService& llm_;
  const IntentRouter* router_ = nullptr;
  ToolExecutor executor_;
};
