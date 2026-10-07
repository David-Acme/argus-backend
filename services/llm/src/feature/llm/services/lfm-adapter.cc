#include "lfm-adapter.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/turn-texts.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <ranges>
#include <string>
#include <string_view>

namespace
{

std::string previousAssistantMessage(const std::vector<ChatMessage>& history)
{
  bool seenUser = false;
  for (const ChatMessage& message : std::views::reverse(history)) {
    if (!seenUser) {
      seenUser = message.role == "user";
      continue;
    }
    if (message.role == "assistant")
      return message.content;
  }
  return {};
}

std::string lastUserMessage(const std::vector<ChatMessage>& history)
{
  for (const ChatMessage& message : std::views::reverse(history))
    if (message.role == "user")
      return LfmAdapter::spokenText(message.content);
  return {};
}

TurnState turnOf(const std::string& utterance, const std::vector<tools::ToolHandle>& tools)
{
  const bool appOffered = std::ranges::any_of(
      tools, [](const tools::ToolHandle& tool) { return isAppTool(tool->spec.name); });
  const bool appAsked = appOffered && asksForAppAction(utterance);
  return {.asked = appAsked || reply_claims::asksForAction(utterance), .appAsked = appAsked, .wrote = false};
}

}

LfmAdapter::LfmAdapter(LlmService& llm, const IntentRouter* router)
    : LfmAdapter(LfmAdapterInput{
          .engine = {.chat = [&llm](const ChatRequest& request) { return llm.chat(request); },
                     .chatStream =
                         [&llm](const ChatRequest& request, TokenCallback onToken) {
                           llm.chatStream(request, std::move(onToken));
                         }},
          .registry = ToolRegistry::instance(),
          .router = router})
{
}

LfmAdapter::LfmAdapter(LfmAdapterInput input)
    : engine_(std::move(input.engine)), executor_(input.registry),
      routerDecider_(input.router), stack_({&ruleDecider_, &routerDecider_}),
      flow_({.executor = executor_,
             .decider = input.decider != nullptr ? input.decider : &stack_,
             .text = input.text != nullptr ? input.text : &ruleText_,
             .policies = std::move(input.policies)})
{
  flow_.useWitnesses({&ruleDecider_, &routerDecider_});
}

std::string LfmAdapter::spokenText(const std::string& content)
{
  std::string_view text(content);
  const auto trimmed = [](std::string_view line) {
    const size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
      return std::string_view();
    const size_t last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
  };
  while (true) {
    const std::string_view body = trimmed(text);
    const size_t lineStart = body.rfind('\n');
    if (lineStart == std::string_view::npos)
      return std::string(body);
    const std::string_view line = trimmed(body.substr(lineStart + 1));
    if (line.size() < 2 || line.front() != '(' || line.back() != ')')
      return std::string(body);
    text = body.substr(0, lineStart);
  }
}

std::string LfmAdapter::lastUtterance(const std::vector<ChatMessage>& history)
{
  return lastUserMessage(history);
}

std::vector<ChatMessage> LfmAdapter::speakMessages(const SpeakInput& args, const std::string& notes) const
{
  std::vector<ChatMessage> msgs = args.history;
  if (!args.input.clock.empty()) {
    for (auto& message : std::views::reverse(msgs)) {
      if (message.role != "user")
        continue;
      message.content += '\n';
      message.content += args.input.clock;
      break;
    }
  }
  if (!notes.empty())
    msgs.push_back({.role = "system", .content = notes});
  return msgs;
}

ToolChatOutput LfmAdapter::chatTurn(const SpeakInput& args)
{
  const ToolChatInput& input = args.input;
  std::vector<ChatMessage>& history = args.history;
  ToolChatOutput output;
  const std::string utterance = lastUserMessage(history);

  ChatRequest req;
  req.maxTokens = input.answerMaxTokens > 0 ? input.answerMaxTokens : 512;
  req.temperature = input.temperature;
  req.resetContext = input.resetContext;
  req.stop = {};
  req.toolCallsAllowed = false;

  if (input.prefillOnly) {
    req.messages = speakMessages(args, {});
    req.prefillOnly = true;
    if (args.onToken != nullptr) {
      engine_.chatStream(req, *args.onToken);
      output.emitted = true;
    }
    else {
      engine_.chat(req);
    }
    return output;
  }

  const std::string previous = previousAssistantMessage(history);
  const turn::Outcome outcome = flow_.run({.utterance = utterance,
                                           .offered = input.tools,
                                           .audience = input.audience,
                                           .context = input.context,
                                           .now = static_cast<int64_t>(std::time(nullptr)),
                                           .previousAssistant = previous});
  for (const auto& step : outcome.steps)
    output.executed.push_back(step.call);
  output.toolMs = outcome.toolMs;
  output.hops = outcome.steps.empty() ? 0 : 1;

  if (outcome.question) {
    output.reply = *outcome.question;
    if (args.onToken != nullptr) {
      (*args.onToken)(output.reply, false);
      (*args.onToken)("", true);
      output.emitted = true;
    }
    history.push_back({.role = "assistant", .content = output.reply});
    return output;
  }

  TurnState state = turnOf(utterance, input.tools);
  state.wrote = outcome.wrote;
  state.opened = outcome.opened;
  state.lang = input.context.lang;
  req.messages = speakMessages(args, turn::TurnFlow::notes(outcome, input.context.lang));

  const auto started = std::chrono::steady_clock::now();
  if (args.onToken != nullptr) {
    reply_claims::ClaimGate gate({.sink = *args.onToken,
                                  .lang = input.context.lang,
                                  .asked = state.asked,
                                  .legitimate = [&state] { return state.wrote; },
                                  .appOnly = state.opened});
    const TokenCallback guarded = gate.callback();
    std::string spoken;
    engine_.chatStream(req, [&spoken, &guarded](const std::string& token, bool done) {
      spoken += token;
      guarded(token, done);
    });
    output.emitted = true;
    output.reply = gate.cut() ? gate.spoken() : spoken;
    if (gate.cut())
      LOG_WARN << "LfmAdapter: a streamed reply claimed something no tool did; it was cut";
  }
  else {
    output.reply = engine_.chat(req);
    if (claimedWithoutTool(output.reply, state)) {
      LOG_WARN << "LfmAdapter: the reply claims something no tool did; answering honestly";
      output.reply = reply_claims::honest(input.context.lang);
    }
  }
  output.generateMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  history.push_back({.role = "assistant", .content = output.reply});
  return output;
}

ToolChatOutput LfmAdapter::chatWithTools(const ToolChatInput& input, std::vector<ChatMessage>& history)
{
  return chatTurn({.input = input, .history = history, .onToken = nullptr});
}

ToolChatOutput LfmAdapter::chatWithToolsStream(const ChatWithToolsStreamInput& args)
{
  return chatTurn({.input = args.input, .history = args.history, .onToken = &args.onToken});
}
