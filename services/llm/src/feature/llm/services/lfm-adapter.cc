#include "lfm-adapter.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>
#include <feature/llm/services/tools/time-arguments.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/turn-texts.hxx>

#include <text/name-match.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <ctime>
#include <ranges>
#include <string>
#include <string_view>

namespace
{

constexpr std::string_view kDefaultLang = "es";

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

std::string missingReadbacks(const turn::Outcome& outcome, const std::string& reply)
{
  const std::string heard = text_norm::folded(reply);
  std::string extra;
  for (const turn::Finding& finding : outcome.findings) {
    if (finding.kind != turn::FindingKind::Done || finding.readback.empty())
      continue;
    std::string core = text_norm::folded(finding.readback);
    for (const std::string_view lead : {"el ", "on "})
      if (core.starts_with(lead))
        core.erase(0, lead.size());
    if (heard.find(core) == std::string::npos)
      extra += " " + finding.readbackSentence;
  }
  return extra;
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

std::string LfmAdapter::replyLang(const ChatRequest& request)
{
  return request.lang.empty() ? std::string(kDefaultLang) : request.lang;
}

std::string LfmAdapter::clockNote(const std::vector<ChatMessage>& history, std::string_view lang)
{
  if (!time_arguments::asksAboutTime(lastUtterance(history)))
    return {};
  return time_arguments::clockLine(static_cast<int64_t>(std::time(nullptr)), std::string(lang));
}

std::vector<ChatMessage> LfmAdapter::spokenMessages(const SpokenMessagesInput& input)
{
  std::vector<ChatMessage> messages = input.history;
  if (!input.clock.empty()) {
    const auto lastUser = std::ranges::find_if(std::views::reverse(messages),
                                               [](const ChatMessage& message) { return message.role == "user"; });
    if (lastUser != std::views::reverse(messages).end())
      messages.insert(std::prev(lastUser.base()), {.role = "system", .content = std::string(input.clock)});
  }
  if (!input.notes.empty())
    messages.push_back({.role = "system", .content = std::string(input.notes)});
  return messages;
}

std::vector<ChatMessage> LfmAdapter::speakMessages(const SpeakInput& args, const std::string& notes) const
{
  return spokenMessages({.history = args.history, .clock = args.input.clock, .notes = notes});
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
  state.called = outcome.called;
  state.lang = input.context.lang;
  req.messages = speakMessages(args, turn::TurnFlow::notes(outcome, input.context.lang));

  const auto started = std::chrono::steady_clock::now();
  if (args.onToken != nullptr) {
    reply_claims::OfferStripGate offers(
        {.sink = *args.onToken, .lang = input.context.lang, .asked = state.asked});
    reply_claims::ClaimGate gate({.sink = offers.callback(),
                                  .lang = input.context.lang,
                                  .asked = state.asked,
                                  .legitimate = [&state] { return state.wrote; },
                                  .appOnly = state.opened,
                                  .callsConfirmed = state.called});
    const TokenCallback guarded = gate.callback();
    std::string spoken;
    std::string raw;
    engine_.chatStream(req, [&](const std::string& token, bool done) {
      raw += token;
      spoken += token;
      if (done && !gate.cut())
        if (const std::string extra = missingReadbacks(outcome, spoken); !extra.empty()) {
          spoken += extra;
          guarded(extra, false);
        }
      guarded(token, done);
    });
    output.emitted = true;
    output.rawReply = std::move(raw);
    output.reply = offers.spoken();
    if (gate.cut())
      LOG_WARN << "LfmAdapter: a streamed reply claimed something no tool did; it was cut";
    if (offers.stripped() > 0)
      LOG_INFO << "LfmAdapter: a trailing generic offer was dropped from the streamed reply";
  }
  else {
    output.reply = engine_.chat(req);
    output.rawReply = output.reply;
    if (claimedWithoutTool(output.reply, state)) {
      LOG_WARN << "LfmAdapter: the reply claims something no tool did; answering honestly";
      output.reply = reply_claims::honest(input.context.lang);
    }
    else {
      output.reply += missingReadbacks(outcome, output.reply);
    }
    reply_claims::StrippedReply stripped =
        reply_claims::withoutTrailingOffer(std::move(output.reply),
                                           {.lang = input.context.lang, .asked = state.asked});
    output.reply = std::move(stripped.text);
    if (stripped.stripped)
      LOG_INFO << "LfmAdapter: a trailing generic offer was dropped from the reply";
  }
  output.generateMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  history.push_back({.role = "assistant", .content = output.reply});
  return output;
}

ToolChatOutput LfmAdapter::chatPlainTurn(const PlainChatInput& input)
{
  const ChatRequest& request = input.request;
  const std::string lang = replyLang(request);
  const std::string utterance = lastUtterance(request.messages);
  const bool asked = reply_claims::asksForAction(utterance);
  ToolChatOutput output;
  ChatRequest req = request;
  req.messages = spokenMessages({.history = request.messages,
                                 .clock = clockNote(request.messages, lang),
                                 .notes = {}});

  const auto started = std::chrono::steady_clock::now();
  if (request.prefillOnly) {
    if (input.onToken != nullptr) {
      engine_.chatStream(req, *input.onToken);
      output.emitted = true;
    }
    else {
      engine_.chat(req);
    }
    return output;
  }

  if (input.onToken != nullptr) {
    reply_claims::OfferStripGate offers({.sink = *input.onToken, .lang = lang, .asked = asked});
    reply_claims::ClaimGate gate({.sink = offers.callback(),
                                  .lang = lang,
                                  .asked = asked,
                                  .legitimate = [] { return false; },
                                  .appOnly = false,
                                  .callsConfirmed = false});
    const TokenCallback sink = request.toolsEnabled ? gate.callback() : offers.callback();
    std::string raw;
    engine_.chatStream(req, [&](const std::string& token, bool done) {
      raw += token;
      sink(token, done);
    });
    output.emitted = true;
    output.rawReply = std::move(raw);
    output.reply = offers.spoken();
    if (request.toolsEnabled && gate.cut())
      LOG_WARN << "LfmAdapter: a streamed reply claimed something no tool did; it was cut";
    if (offers.stripped() > 0)
      LOG_INFO << "LfmAdapter: a trailing generic offer was dropped from the streamed reply";
  }
  else {
    output.rawReply = engine_.chat(req);
    output.reply = output.rawReply;
    if (request.toolsEnabled)
      output.reply = reply_claims::withoutFalseClaims(
          {.text = std::move(output.reply), .utterance = utterance, .lang = lang});
    reply_claims::StrippedReply stripped =
        reply_claims::withoutTrailingOffer(std::move(output.reply), {.lang = lang, .asked = asked});
    output.reply = std::move(stripped.text);
    if (stripped.stripped)
      LOG_INFO << "LfmAdapter: a trailing generic offer was dropped from the reply";
  }
  output.generateMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  return output;
}

ToolChatOutput LfmAdapter::chatPlain(const ChatRequest& request)
{
  return chatPlainTurn({.request = request, .onToken = nullptr});
}

ToolChatOutput LfmAdapter::chatPlainStream(const PlainChatStreamInput& input)
{
  return chatPlainTurn({.request = input.request, .onToken = &input.onToken});
}

ToolChatOutput LfmAdapter::chatWithTools(const ToolChatInput& input, std::vector<ChatMessage>& history)
{
  return chatTurn({.input = input, .history = history, .onToken = nullptr});
}

ToolChatOutput LfmAdapter::chatWithToolsStream(const ChatWithToolsStreamInput& args)
{
  return chatTurn({.input = args.input, .history = args.history, .onToken = &args.onToken});
}
