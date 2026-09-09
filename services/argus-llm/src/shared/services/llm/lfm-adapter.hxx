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
  // Tool-bearing hops run cold: the f8-b1 gate measured 11/20 memory_save at
  // temperature 0 against 6/20 at the conversational default.
  float toolTemperature = 0.0F;
  // Hop one honors the caller's session reset; later hops continue the context.
  bool resetContext = false;
  // Token cap for every generation, hops and prose alike — a no-tool reply IS
  // hop one, so the cap the wire asks for must bound it. 0 keeps 512.
  int32_t answerMaxTokens = 0;
};

struct ToolChatOutput
{
  std::string reply;
  std::vector<tools::ToolCall> executed;
  int hops = 0;
  // The stream leg already put `reply` on the wire token by token.
  bool emitted = false;
  // Where the turn spent its time: model generation against tool execution.
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

// Tool-calling adapter over LlmService; parses raw JSON, pythonic and marker-wrapped calls.
// With a router attached the classifier picks the tool and the model only
// writes the prose; without one, or when the router abstains, the hop loop
// runs exactly as it did before.
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

  // True while `text` can still be the opening of a tool call, so a streaming
  // hop must keep holding it back. Every call the f8-b4 probes measured led
  // the reply with the sentinel; a bare pythonic or JSON call leads with its
  // bracket.
  static bool mayOpenToolCall(const std::string& text);

  ToolChatOutput chatWithTools(const ToolChatInput& input,
                               std::vector<ChatMessage>& history);

  // Same loop, streaming every hop: tokens are held only until the reply can
  // no longer open a tool call, then they flow live.
  ToolChatOutput chatWithToolsStream(const ToolChatInput& input,
                                     std::vector<ChatMessage>& history,
                                     const TokenCallback& onToken);

private:
  // The routed path: a confident router runs the tool it picked and answers
  // in one prose generation, skipping the tool hop entirely. True when it
  // took the turn; false hands the turn to toolHops untouched.
  bool routedTurn(ToolHopContext ctx);

  // Hops with tool declarations until the model answers in prose. True when
  // `ctx.output.reply` holds that answer; false when the hops exhausted and
  // a final prose hop is needed.
  bool toolHops(ToolHopContext ctx, const std::string& declarations);

  // The turn's closing generation: no declarations, so raw tool output never
  // reaches the caller. Streams when the context carries a callback. The
  // routed path runs it cold (it confirms a completed action), the exhausted
  // loop at the caller's conversational temperature.
  void proseAnswer(ToolHopContext ctx, float temperature);

  // Streams a hop that might still turn out to be a tool call: tokens are
  // held while the text can still be a call's opening, released live once it
  // cannot. `streamed` reports whether any byte reached the wire.
  std::string streamHop(const ChatRequest& request, const TokenCallback& onToken,
                        bool& streamed);

  LlmService& llm_;
  const IntentRouter* router_ = nullptr;
  ToolExecutor executor_;
};
