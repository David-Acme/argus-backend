#include "lfm-adapter.hxx"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

constexpr const char* kToolOpen = "<|tool_call_start|>";
constexpr const char* kToolClose = "<|tool_call_end|>";

std::string jsonType(const tools::ToolArgumentSpec& spec)
{
  if (spec.type == "number")
    return "number";
  if (spec.type == "boolean")
    return "boolean";
  return "string";
}

Json::Value bareValue(const std::string& token)
{
  if (token == "True" || token == "true")
    return Json::Value(true);
  if (token == "False" || token == "false")
    return Json::Value(false);
  try {
    std::size_t used = 0;
    if (token.find('.') != std::string::npos) {
      const double number = std::stod(token, &used);
      if (used == token.size())
        return Json::Value(number);
    }
    else {
      const long long number = std::stoll(token, &used);
      if (used == token.size())
        return Json::Value(Json::Int64(number));
    }
  }
  catch (...) {
  }
  return Json::Value(token);
}

std::string trimKey(std::string_view raw)
{
  static constexpr std::string_view junk = " \t\r\n\",'";
  const size_t begin = raw.find_first_not_of(junk);
  if (begin == std::string_view::npos)
    return {};
  const size_t end = raw.find_last_not_of(junk);
  return std::string(raw.substr(begin, end - begin + 1));
}

class PythonicScanner
{
public:
  explicit PythonicScanner(std::string_view text) : text_(text) {}

  std::vector<tools::ToolCall> calls()
  {
    std::vector<tools::ToolCall> out;
    size_t from = 0;
    while (true) {
      const size_t open = text_.find('[', from);
      if (open == std::string_view::npos)
        break;
      at_ = open + 1;
      std::vector<tools::ToolCall> listed;
      if (callList(listed)) {
        for (auto& call : listed)
          out.push_back(std::move(call));
        from = at_;
      }
      else {
        from = open + 1;
      }
    }
    return out;
  }

private:
  [[nodiscard]] bool done() const { return at_ >= text_.size(); }

  void skip(std::string_view chars)
  {
    while (!done() && chars.find(text_[at_]) != std::string_view::npos)
      ++at_;
  }

  bool callList(std::vector<tools::ToolCall>& out)
  {
    while (true) {
      skip(" \t\r\n,");
      if (done())
        return false;
      if (text_[at_] == ']') {
        ++at_;
        return !out.empty();
      }
      tools::ToolCall call;
      if (!callName(call.name) || !callArguments(call.arguments))
        return false;
      out.push_back(std::move(call));
    }
  }

  bool callName(std::string& name)
  {
    const size_t begin = at_;
    while (!done() && (std::isalnum(static_cast<unsigned char>(text_[at_])) != 0 ||
                       text_[at_] == '.' || text_[at_] == '_' || text_[at_] == '-'))
      ++at_;
    name = std::string(text_.substr(begin, at_ - begin));
    skip(" \t");
    if (name.empty() || done() || text_[at_] != '(')
      return false;
    ++at_;
    return true;
  }

  bool callArguments(Json::Value& arguments)
  {
    arguments = Json::Value(Json::objectValue);
    while (true) {
      skip(" \t\r\n,");
      if (done())
        return false;
      if (text_[at_] == ')') {
        ++at_;
        return true;
      }
      const size_t keyBegin = at_;
      while (!done() && text_[at_] != '=' && text_[at_] != ')')
        ++at_;
      if (done())
        return false;
      if (text_[at_] == ')')
        continue;
      const std::string key = trimKey(text_.substr(keyBegin, at_ - keyBegin));
      ++at_;
      skip(" \t");
      if (done())
        return false;
      Json::Value value;
      if (!argumentValue(value))
        return false;
      if (!key.empty() && !value.isNull())
        arguments[key] = std::move(value);
    }
  }

  bool argumentValue(Json::Value& value)
  {
    const char first = text_[at_];
    if (first == '"' || first == '\'')
      return quoted(value);
    if (first == '{' || first == '[')
      return nested(value);
    const size_t begin = at_;
    while (!done() && text_[at_] != ',' && text_[at_] != ')')
      ++at_;
    const std::string token = trimKey(text_.substr(begin, at_ - begin));
    if (token != "None" && token != "null")
      value = bareValue(token);
    return true;
  }

  bool quoted(Json::Value& value)
  {
    const char quote = text_[at_++];
    std::string decoded;
    while (!done() && text_[at_] != quote) {
      char c = text_[at_++];
      if (c == '\\' && !done()) {
        c = text_[at_++];
        if (c == 'n')
          c = '\n';
        else if (c == 'r')
          c = '\r';
        else if (c == 't')
          c = '\t';
      }
      decoded += c;
    }
    if (done())
      return false;
    ++at_;
    value = Json::Value(decoded);
    return true;
  }

  bool nested(Json::Value& value)
  {
    const size_t begin = at_;
    int depth = 0;
    char quote = 0;
    for (; !done(); ++at_) {
      const char c = text_[at_];
      if (quote != 0) {
        if (c == '\\')
          ++at_;
        else if (c == quote)
          quote = 0;
        continue;
      }
      if (c == '"' || c == '\'')
        quote = c;
      else if (c == '{' || c == '[')
        ++depth;
      else if ((c == '}' || c == ']') && --depth == 0) {
        ++at_;
        const std::string raw(text_.substr(begin, at_ - begin));
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream in(raw);
        if (!Json::parseFromStream(builder, in, &value, &errors))
          value = Json::Value(raw);
        return true;
      }
    }
    return false;
  }

  std::string_view text_;
  size_t at_{0};
};

std::vector<tools::ToolCall> parsePythonic(const std::string& text)
{
  return PythonicScanner(text).calls();
}

struct TryJsonInput
{
  const std::string& text;
  size_t open{0};
  size_t close{0};
};

std::optional<tools::ToolCall> tryJson(const TryJsonInput& input)
{
  const std::string& text = input.text;

  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream in(
      text.substr(input.open, input.close - input.open + 1));
  if (!Json::parseFromStream(builder, in, &value, &errors) ||
      !value.isObject() || !value.isMember("name") || !value["name"].isString())
    return std::nullopt;
  tools::ToolCall call;
  call.name = value["name"].asString();
  call.arguments = value.isMember("arguments") && value["arguments"].isObject()
                       ? value["arguments"]
                       : Json::Value(Json::objectValue);
  return call;
}

std::vector<tools::ToolCall> parseJsonCalls(const std::string& text)
{
  std::vector<tools::ToolCall> out;
  size_t pos = 0;
  while (true) {
    const size_t open = text.find('{', pos);
    if (open == std::string::npos)
      break;
    pos = open + 1;
    int depth = 1;
    bool inString = false;
    bool escaped = false;
    size_t close = std::string::npos;
    for (size_t i = open + 1; i < text.size(); ++i) {
      const char c = text[i];
      if (inString) {
        if (escaped)
          escaped = false;
        else if (c == '\\')
          escaped = true;
        else if (c == '"')
          inString = false;
        continue;
      }
      if (c == '"')
        inString = true;
      else if (c == '{')
        ++depth;
      else if (c == '}') {
        --depth;
        if (depth == 0) {
          close = i;
          break;
        }
      }
    }
    if (close == std::string::npos)
      break;
    if (const auto call =
            tryJson({.text = text, .open = open, .close = close})) {
      bool seen = false;
      for (const auto& existing : out) {
        if (existing.name == call->name)
          seen = true;
      }
      if (!seen)
        out.push_back(*call);
    }
    pos = close + 1;
  }
  return out;
}

struct HopMessagesInput
{
  const std::vector<ChatMessage>& history;
  const std::string& system;
  const std::string& declarations;
};

std::vector<ChatMessage> hopMessages(const HopMessagesInput& input)
{
  const std::vector<ChatMessage>& history = input.history;
  const std::string& system = input.system;
  const std::string& declarations = input.declarations;

  std::vector<ChatMessage> msgs = history;
  std::string content = system;
  if (!declarations.empty())
    content += "\nList of tools: " + declarations;
  if (!msgs.empty() && msgs.front().role == "system")
    msgs.front().content += "\n" + content;
  else
    msgs.insert(msgs.begin(),
                ChatMessage{.role = "system", .content = content});
  return msgs;
}

}

std::string LfmAdapter::buildToolDeclarations(
    const std::vector<const tools::ToolDescriptor*>& tools)
{
  std::string out = "[";
  bool first = true;
  for (const auto* tool : tools) {
    if (!first)
      out += ", ";
    first = false;
    Json::Value decl(Json::objectValue);
    decl["name"] = tool->name;
    decl["description"] = tool->description;
    Json::Value parameters(Json::objectValue);
    parameters["type"] = "object";
    Json::Value properties(Json::objectValue);
    Json::Value required(Json::arrayValue);
    for (const auto& spec : tool->arguments) {
      Json::Value prop(Json::objectValue);
      prop["type"] = jsonType(spec);
      if (!spec.enumValues.empty()) {
        Json::Value allowed(Json::arrayValue);
        for (const auto& value : spec.enumValues)
          allowed.append(value);
        prop["enum"] = allowed;
      }
      properties[spec.name] = prop;
      if (spec.required)
        required.append(spec.name);
    }
    parameters["properties"] = properties;
    if (!required.empty())
      parameters["required"] = required;
    decl["parameters"] = parameters;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    out += Json::writeString(builder, decl);
  }
  out += "]";
  return out;
}

std::vector<tools::ToolCall> LfmAdapter::parseToolCalls(const std::string& text)
{
  std::vector<tools::ToolCall> calls;

  size_t pos = 0;
  while (true) {
    const size_t open = text.find(kToolOpen, pos);
    if (open == std::string::npos)
      break;
    const size_t body = open + strlen(kToolOpen);
    const size_t close = text.find(kToolClose, body);
    if (close == std::string::npos)
      break;
    const std::string block = text.substr(body, close - body);
    pos = close + strlen(kToolClose);

    bool parsed = false;
    if (const auto call =
            tryJson({.text = block, .open = 0, .close = block.size() - 1})) {
      calls.push_back(*call);
      parsed = true;
    }
    else {
      const size_t first = block.find_first_not_of(" \t\r\n");
      const bool bare = first != std::string::npos && block[first] != '[';
      const auto pythonic = parsePythonic(bare ? "[" + block + "]" : block);
      if (!pythonic.empty()) {
        for (const auto& call : pythonic)
          calls.push_back(call);
        parsed = true;
      }
    }
    if (!parsed)
      LOG_WARN << "LfmAdapter: dropped malformed tool block";
  }

  const auto jsonCalls = parseJsonCalls(text);
  for (const auto& call : jsonCalls) {
    bool seen = false;
    for (const auto& existing : calls) {
      if (existing.name == call.name)
        seen = true;
    }
    if (!seen)
      calls.push_back(call);
  }

  const auto pythonic = parsePythonic(text);
  for (const auto& call : pythonic) {
    bool seen = false;
    for (const auto& existing : calls) {
      if (existing.name == call.name)
        seen = true;
    }
    if (!seen)
      calls.push_back(call);
  }
  return calls;
}

bool LfmAdapter::mayOpenToolCall(const std::string& text)
{
  const std::string_view open(kToolOpen);
  const size_t seen = std::min(text.size(), open.size());
  if (text.compare(0, seen, open.substr(0, seen)) == 0)
    return true;
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return true;
  return text[first] == '[' || text[first] == '{';
}

std::string LfmAdapter::streamHop(const StreamHopInput& input)
{
  std::string reply;
  std::string held;
  input.streamed = false;
  engine_.chatStream(input.request, [&](const std::string& token, bool done) {
    if (done) {
      if (input.streamed)
        input.onToken("", true);
      return;
    }
    reply += token;
    if (input.streamed) {
      input.onToken(token, false);
      return;
    }
    held += token;
    if (mayOpenToolCall(held))
      return;
    input.streamed = true;
    input.onToken(held, false);
  });
  return reply;
}

namespace
{

std::string lastUserMessage(const std::vector<ChatMessage>& history)
{
  const auto found = std::find_if(
      history.rbegin(), history.rend(),
      [](const ChatMessage& message) { return message.role == "user"; });
  return found == history.rend() ? std::string()
                                 : LfmAdapter::spokenText(found->content);
}

std::optional<tools::ToolCall> routedCall(intent::ToolIntent decided,
                                          const std::string& utterance)
{
  tools::ToolCall call;
  call.arguments = Json::Value(Json::objectValue);
  switch (decided) {
    case intent::ToolIntent::MemorySave:
      call.name = "memory.remember";
      call.arguments["text"] = utterance;
      break;
    case intent::ToolIntent::MemoryRecall:
      call.name = "memory.recall";
      call.arguments["query"] = utterance;
      break;
    case intent::ToolIntent::ReminderSet:
      call.name = "memory.remind";
      call.arguments["text"] = utterance;
      break;
    case intent::ToolIntent::MemoryForget:
      call.name = "memory.forget";
      call.arguments["query"] = utterance;
      break;
    default:
      return std::nullopt;
  }
  return call;
}

std::string quotedArgument(const std::string& value)
{
  std::string out = "'";
  for (const char c : value) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\'':
        out += "\\'";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
    }
  }
  out += '\'';
  return out;
}

std::string renderedArgument(const Json::Value& value)
{
  if (value.isString())
    return quotedArgument(value.asString());
  if (value.isBool())
    return value.asBool() ? "True" : "False";
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
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
    : engine_(std::move(input.engine)), registry_(input.registry),
      router_(input.router), executor_(input.registry)
{
}

bool LfmAdapter::offered(const tools::ToolCall& call,
                         const std::vector<const tools::ToolDescriptor*>& tools) const
{
  const auto* descriptor = registry_.find(call.name);
  return descriptor != nullptr && std::ranges::find(tools, descriptor) != tools.end();
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

std::string LfmAdapter::renderToolCall(const tools::ToolCall& call)
{
  std::string rendered = std::string(kToolOpen) + "[" + call.name + "(";
  bool first = true;
  if (call.arguments.isObject()) {
    for (const auto& name : call.arguments.getMemberNames()) {
      if (!first)
        rendered += ", ";
      first = false;
      rendered += name + "=" + renderedArgument(call.arguments[name]);
    }
  }
  rendered += ")]";
  rendered += kToolClose;
  return rendered;
}

bool LfmAdapter::routedTurn(ToolHopContext ctx)
{
  if (router_ == nullptr)
    return false;
  const std::string utterance = lastUserMessage(ctx.history);
  if (utterance.empty())
    return false;

  const intent::IntentDecision decision =
      router_->decide(utterance, ctx.input.context.lang);
  if (!decision.confident)
    return false;

  auto call = routedCall(decision.intent, utterance);
  if (!call || !offered(*call, ctx.input.tools))
    return false;

  call->context = ctx.input.context;
  call->context.utterance = utterance;
  call->context.decided = true;

  const auto toolStart = std::chrono::steady_clock::now();
  const tools::ToolResult executed = executor_.execute(*call, ctx.input.role);
  ctx.output.toolMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - toolStart)
                          .count();
  if (!executed.ok && decision.intent == intent::ToolIntent::MemoryForget) {
    LOG_WARN << "LfmAdapter: routed tool '" << call->name
             << "' failed: " << executed.output << "; falling back to the "
             << "tool loop";
    return false;
  }

  LOG_INFO << "LfmAdapter: router picked '" << call->name << "' ("
           << intent::toolIntentToString(decision.intent) << " score "
           << decision.score << (decision.fromRules ? ", rules" : ", model")
           << (executed.ok ? "): " : ") and it failed: ") << executed.output;
  ctx.output.executed.push_back(*call);
  ctx.output.hops = 1;
  ctx.history.push_back({.role = "assistant", .content = renderToolCall(*call)});
  ctx.history.push_back({.role = "tool", .content = executed.output});
  proseAnswer(ctx, ctx.input.toolTemperature);
  return true;
}

void LfmAdapter::proseAnswer(ToolHopContext ctx, float temperature)
{
  ChatRequest req;
  req.messages = hopMessages({.history = ctx.history,
                              .system = ctx.input.systemPrompt,
                              .declarations = ctx.declarations});
  req.maxTokens =
      ctx.input.answerMaxTokens > 0 ? ctx.input.answerMaxTokens : 512;
  req.temperature = temperature;
  req.resetContext = false;
  req.stop = {};
  req.toolCallsAllowed = false;

  const auto genStart = std::chrono::steady_clock::now();
  std::string reply;
  if (ctx.onToken != nullptr) {
    const TokenCallback& onToken = *ctx.onToken;
    engine_.chatStream(req, [&reply, &onToken](const std::string& token,
                                            bool done) {
      reply += token;
      onToken(token, done);
    });
    ctx.output.emitted = true;
  }
  else {
    reply = engine_.chat(req);
  }
  ctx.output.generateMs +=
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - genStart)
          .count();
  ctx.output.reply = reply;
  ctx.history.push_back({.role = "assistant", .content = reply});
}

ChatRequest LfmAdapter::hopRequest(const ToolHopContext& ctx) const
{
  ChatRequest req;
  req.messages = hopMessages({.history = ctx.history,
                              .system = ctx.input.systemPrompt,
                              .declarations = ctx.declarations});
  req.maxTokens = ctx.input.answerMaxTokens > 0 ? ctx.input.answerMaxTokens : 512;
  req.temperature = ctx.input.toolTemperature;
  req.resetContext = ctx.output.hops == 0 && ctx.input.resetContext;
  if (!ctx.declarations.empty())
    req.stop = {kToolClose};
  req.prefillOnly = ctx.input.prefillOnly;
  return req;
}

bool LfmAdapter::toolHops(ToolHopContext ctx)
{
  const ToolChatInput& input = ctx.input;
  std::vector<ChatMessage>& history = ctx.history;
  ToolChatOutput& output = ctx.output;
  const TokenCallback* onToken = ctx.onToken;
  std::vector<tools::ToolResult> succeeded;
  for (output.hops = 0; output.hops < input.maxHops; ++output.hops) {
    const ChatRequest req = hopRequest(ctx);

    bool streamed = false;
    const auto genStart = std::chrono::steady_clock::now();
    const std::string reply =
        onToken
            ? streamHop({.request = req, .onToken = *onToken, .streamed = streamed})
            : engine_.chat(req);
    output.generateMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - genStart)
                             .count();
    history.push_back({.role = "assistant", .content = reply});

    std::vector<tools::ToolCall> calls;
    bool attempted = false;
    if (!streamed) {
      attempted = reply.find(kToolOpen) != std::string::npos;
      for (auto& call : parseToolCalls(reply)) {
        attempted = true;
        if (offered(call, input.tools))
          calls.push_back(std::move(call));
        else
          LOG_WARN << "LfmAdapter: dropped a call to '" << call.name
                   << "', a tool this turn does not offer";
      }
    }
    if (calls.empty()) {
      if (attempted) {
        history.pop_back();
        LOG_INFO << "LfmAdapter: the reply held no call this turn can run; "
                    "answering in prose";
        return false;
      }
      output.reply = reply;
      output.emitted = streamed;
      return true;
    }

    const std::string utterance = lastUserMessage(history);
    bool ranAny = false;
    for (auto call : calls) {
      const auto earlier = std::ranges::find(succeeded, call.name, &tools::ToolResult::tool);
      if (earlier != succeeded.end()) {
        LOG_INFO << "LfmAdapter: '" << call.name
                 << "' already ran this turn; its result is reused";
        history.push_back({.role = "tool", .content = earlier->output});
        continue;
      }
      ranAny = true;
      call.context = input.context;
      call.context.utterance = utterance;
      const auto toolStart = std::chrono::steady_clock::now();
      const auto executed = executor_.execute(call, input.role);
      output.toolMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - toolStart)
                           .count();
      if (executed.ok) {
        LOG_INFO << "LfmAdapter: tool '" << call.name
                 << "' ok: " << executed.output;
        succeeded.push_back(executed);
      }
      else {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        LOG_WARN << "LfmAdapter: tool '" << call.name
                 << "' failed: " << executed.output
                 << " args: " << Json::writeString(builder, call.arguments);
      }
      output.executed.push_back(call);
      history.push_back({.role = "tool", .content = executed.output});
    }
    if (!ranAny) {
      LOG_INFO << "LfmAdapter: the model repeated a call it already made; "
                  "answering in prose";
      return false;
    }
  }
  return false;
}

ToolChatOutput LfmAdapter::chatWithTools(const ToolChatInput& input,
                                         std::vector<ChatMessage>& history)
{
  ToolChatOutput output;
  const std::string declarations = input.tools.empty()
                                       ? std::string()
                                       : buildToolDeclarations(input.tools);
  const ToolHopContext ctx{.input = input,
                           .history = history,
                           .output = output,
                           .onToken = nullptr,
                           .declarations = declarations};

  if (input.prefillOnly) {
    engine_.chat(hopRequest(ctx));
    return output;
  }
  if (routedTurn(ctx))
    return output;
  if (toolHops(ctx))
    return output;

  LOG_WARN << "LfmAdapter: tool loop ended after " << output.hops
           << " hops without a prose answer";
  proseAnswer(ctx, input.temperature);
  return output;
}

ToolChatOutput LfmAdapter::chatWithToolsStream(
    const ChatWithToolsStreamInput& args)
{
  const ToolChatInput& input = args.input;
  std::vector<ChatMessage>& history = args.history;
  const TokenCallback& onToken = args.onToken;

  ToolChatOutput output;
  const std::string declarations = input.tools.empty()
                                       ? std::string()
                                       : buildToolDeclarations(input.tools);
  const ToolHopContext ctx{.input = input,
                           .history = history,
                           .output = output,
                           .onToken = &onToken,
                           .declarations = declarations};

  if (input.prefillOnly) {
    engine_.chatStream(hopRequest(ctx), onToken);
    output.emitted = true;
    return output;
  }
  if (routedTurn(ctx))
    return output;

  if (toolHops(ctx)) {
    if (!output.emitted) {
      onToken(output.reply, false);
      onToken("", true);
    }
    return output;
  }

  LOG_WARN << "LfmAdapter: tool loop ended after " << output.hops
           << " hops without a prose answer";
  proseAnswer(ctx, input.temperature);
  return output;
}
