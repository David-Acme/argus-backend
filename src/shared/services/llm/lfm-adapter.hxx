#pragma once

#include <shared/contracts/tool-contracts.hxx>
#include <shared/services/llm/llm-service.hxx>
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
  // Sampling for the tool loop. -1 = LlmService default ([llm] temperature).
  float temperature = -1.0F;
};

struct ToolChatOutput
{
  std::string reply;
  std::vector<tools::ToolCall> executed;
  int hops = 0;
};

// Tool-calling adapter over LlmService. The LFM2.5 chat template (extracted
// from the GGUF) is ChatML-based and declares tools inside the system
// message as "List of tools: [...]"; it has no <|tool_*|> tokens, so the
// prompt never contains literal special tokens (llama_tokenize would parse
// them as real special tokens inside the system message and suppress
// generation). The model emits the tool call as raw JSON
// ({"name": ..., "arguments": {...}}); the parser also accepts pythonic
// [name(arg="v", ...)] and marker-wrapped blocks. Tool results are appended
// as role "tool" messages (append-only, so prefix reuse in
// LlmService::prefill survives).
class LfmAdapter
{
public:
  explicit LfmAdapter(LlmService& llm) : llm_(llm) {}

  static std::string
  buildToolDeclarations(const std::vector<const tools::ToolDescriptor*>& tools);
  static std::vector<tools::ToolCall> parseToolCalls(const std::string& text);

  ToolChatOutput chatWithTools(const ToolChatInput& input,
                               std::vector<ChatMessage>& history);

private:
  LlmService& llm_;
};
