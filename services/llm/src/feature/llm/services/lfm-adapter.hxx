#pragma once

#include <cstdint>
#include <shared/vocabulary/tool-contracts.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <llm/llm-service.hxx>
#include <feature/llm/services/tools/claim-check.hxx>
#include <feature/llm/services/tools/tool-access.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/turn/context-selector.hxx>
#include <feature/llm/services/turn/deciders.hxx>
#include <feature/llm/services/turn/decision-policy.hxx>
#include <feature/llm/services/turn/slots.hxx>
#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/turn-flow.hxx>
#include <functional>
#include <string>
#include <vector>

struct ToolChatInput
{
  std::vector<tools::ToolHandle> tools;
  ToolAudience audience;
  tools::ToolContext context;
  std::vector<ContextFact> contextFacts;
  std::string clock{};
  float temperature = -1.0F;
  bool resetContext = false;
  int32_t answerMaxTokens = 0;
  bool prefillOnly = false;
};

struct ToolChatOutput
{
  std::string reply;
  std::string rawReply;
  std::string contextBlock;
  std::string speech;
  std::string act;
  std::vector<tools::ToolCall> executed;
  int hops = 0;
  bool emitted = false;
  int64_t generateMs = 0;
  int64_t toolMs = 0;
};

struct ActSpeakInput
{
  const ToolChatInput& input;
  std::vector<ChatMessage>& history;
  const TokenCallback* onToken{nullptr};
  const turn::speech::Speech& speech;
  std::string tail;
  bool asked{false};
  bool wrote{false};
  bool opened{false};
  bool callsConfirmed{false};
};

struct SpokenMessagesInput
{
  const std::vector<ChatMessage>& history;
  std::string_view clock;
  std::string_view notes;
};

struct PlainChatInput
{
  const ChatRequest& request;
  const TokenCallback* onToken{nullptr};
};

struct PlainChatStreamInput
{
  const ChatRequest& request;
  const TokenCallback& onToken;
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

  static std::string replyLang(const ChatRequest& request);

  static std::string clockNote(const std::vector<ChatMessage>& history, std::string_view lang);

  static std::vector<ChatMessage> spokenMessages(const SpokenMessagesInput& input);

  ToolChatOutput chatWithTools(const ToolChatInput& input, std::vector<ChatMessage>& history);

  ToolChatOutput chatWithToolsStream(const ChatWithToolsStreamInput& args);

  ToolChatOutput chatPlain(const ChatRequest& request);

  ToolChatOutput chatPlainStream(const PlainChatStreamInput& input);

  ToolExecutor& executor() { return executor_; }

  turn::TurnFlow& flow() { return flow_; }

  [[nodiscard]] const turn::RuleDecider& ruleDecider() const { return ruleDecider_; }

  [[nodiscard]] const turn::RouterDecider& routerDecider() const { return routerDecider_; }

private:
  ToolChatOutput chatTurn(const SpeakInput& args);

  ToolChatOutput chatPlainTurn(const PlainChatInput& input);

  [[nodiscard]] std::vector<ChatMessage> speakMessages(const SpeakInput& args, const std::string& notes) const;

  void speakAct(const ActSpeakInput& input, ToolChatOutput& output);

  ChatEngine engine_;
  ToolExecutor executor_;
  turn::RuleDecider ruleDecider_;
  turn::RouterDecider routerDecider_;
  turn::FirstOf stack_;
  turn::ContextSelector contextSelector_;
  slots::RuleText ruleText_;
  turn::TurnFlow flow_;
};
