#pragma once

#include <shared/contracts/tool-contracts.hxx>
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
};

struct ToolChatOutput
{
  std::string reply;
  std::vector<tools::ToolCall> executed;
  int hops = 0;
};

// Tool-calling adapter over LlmService; parses raw JSON, pythonic and marker-wrapped calls.
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
