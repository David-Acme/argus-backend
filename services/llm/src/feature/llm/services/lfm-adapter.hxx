#pragma once

#include <cstdint>
#include <shared/vocabulary/tool-contracts.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <llm/llm-service.hxx>
#include <feature/llm/services/tools/tool-access.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/turn/deciders.hxx>
#include <feature/llm/services/turn/decision-policy.hxx>
#include <feature/llm/services/turn/slots.hxx>
#include <feature/llm/services/turn/turn-flow.hxx>
#include <functional>
#include <string>
#include <vector>

struct ToolChatInput
{
  std::vector<tools::ToolHandle> tools;
  ToolAudience audience;
  tools::ToolContext context;
  std::string clock{};
  float temperature = -1.0F;
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
};

struct ChatWithToolsStreamInput
{
  const ToolChatInput& input;
  std::vector<ChatMessage>& history;
  const TokenCallback& onToken;
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
  const turn::Decider* decider{nullptr};
  const slots::TextSlots* text{nullptr};
  turn::PolicySet policies{};
};

struct SpeakInput
{
  const ToolChatInput& input;
  std::vector<ChatMessage>& history;
  const TokenCallback* onToken{nullptr};
};

class LfmAdapter
{
public:
  explicit LfmAdapter(LlmService& llm, const IntentRouter* router = nullptr);

  explicit LfmAdapter(LfmAdapterInput input);

  static std::string spokenText(const std::string& content);

  static std::string lastUtterance(const std::vector<ChatMessage>& history);

  ToolChatOutput chatWithTools(const ToolChatInput& input, std::vector<ChatMessage>& history);

  ToolChatOutput chatWithToolsStream(const ChatWithToolsStreamInput& args);

  ToolExecutor& executor() { return executor_; }

  turn::TurnFlow& flow() { return flow_; }

private:
  ToolChatOutput chatTurn(const SpeakInput& args);

  [[nodiscard]] std::vector<ChatMessage> speakMessages(const SpeakInput& args, const std::string& notes) const;

  ChatEngine engine_;
  ToolExecutor executor_;
  turn::RuleDecider ruleDecider_;
  turn::RouterDecider routerDecider_;
  turn::FirstOf stack_;
  slots::RuleText ruleText_;
  turn::TurnFlow flow_;
};
