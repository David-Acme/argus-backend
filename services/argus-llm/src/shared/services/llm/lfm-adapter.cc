#include "lfm-adapter.hxx"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>
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

// Bare pythonic tokens keep the schema's types: a full number parse becomes one.
Json::Value bareValue(const std::string& token)
{
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

// Model emissions trail separators into keys; strip the junk so arguments land.
std::string trimKey(const std::string& raw)
{
  static const std::string junk = " \t\",";
  const size_t begin = raw.find_first_not_of(junk);
  if (begin == std::string::npos)
    return {};
  const size_t end = raw.find_last_not_of(junk);
  return raw.substr(begin, end - begin + 1);
}

// Parses pythonic `[name(arg="v", ...)]` calls, one or more.
std::vector<tools::ToolCall> parsePythonic(const std::string& text)
{
  std::vector<tools::ToolCall> out;
  size_t pos = 0;
  while (true) {
    const size_t open = text.find('[', pos);
    if (open == std::string::npos)
      break;
    const size_t close = text.find(']', open);
    if (close == std::string::npos)
      break;
    const std::string block = text.substr(open + 1, close - open - 1);
    pos = close + 1;

    size_t start = 0;
    while (start < block.size()) {
      const size_t paren = block.find('(', start);
      if (paren == std::string::npos)
        break;
      const size_t end = block.find(')', paren);
      if (end == std::string::npos)
        break;
      const std::string name = block.substr(start, paren - start);
      const std::string body = block.substr(paren + 1, end - paren - 1);
      start = end + 1;

      std::string trimmed = name;
      while (!trimmed.empty() &&
             (trimmed.front() == ' ' || trimmed.front() == ','))
        trimmed.erase(trimmed.begin());
      while (!trimmed.empty() &&
             (trimmed.back() == ' ' || trimmed.back() == ','))
        trimmed.pop_back();
      if (trimmed.empty())
        continue;

      tools::ToolCall call;
      call.name = trimmed;
      Json::Value args(Json::objectValue);
      size_t argPos = 0;
      while (argPos < body.size()) {
        const size_t eq = body.find('=', argPos);
        if (eq == std::string::npos)
          break;
        std::string key = trimKey(body.substr(argPos, eq - argPos));
        size_t valueStart = eq + 1;
        while (valueStart < body.size() && body[valueStart] == ' ')
          ++valueStart;
        if (valueStart >= body.size())
          break;
        std::string value;
        if (body[valueStart] == '"' || body[valueStart] == '\'') {
          const char quote = body[valueStart];
          ++valueStart;
          while (valueStart < body.size() && body[valueStart] != quote) {
            if (body[valueStart] == '\\' && valueStart + 1 < body.size())
              ++valueStart;
            value += body[valueStart];
            ++valueStart;
          }
          argPos = valueStart < body.size() ? valueStart + 1 : body.size();
        }
        else {
          while (valueStart < body.size() && body[valueStart] != ',')
            value += body[valueStart++];
          argPos = valueStart;
          args[key] = bareValue(value);
          continue;
        }
        args[key] = value;
        while (argPos < body.size() && body[argPos] != ',')
          ++argPos;
        if (argPos < body.size())
          ++argPos;
      }
      call.arguments = args;
      out.push_back(std::move(call));
    }
  }
  return out;
}

std::optional<tools::ToolCall> tryJson(const std::string& text, size_t open,
                                       size_t close)
{
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream in(text.substr(open, close - open + 1));
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

// Balanced-brace scan for a JSON object at/open positions.
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
    if (const auto call = tryJson(text, open, close)) {
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

// One hop's prompt: the tool policy rides the caller's own system message.
std::vector<ChatMessage>
hopMessages(const std::vector<ChatMessage>& history, const std::string& system,
            const std::string& declarations)
{
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

} // namespace

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
    if (const auto call = tryJson(block, 0, block.size() - 1)) {
      calls.push_back(*call);
      parsed = true;
    }
    else {
      const auto pythonic = parsePythonic(block);
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

std::string LfmAdapter::streamHop(const ChatRequest& request,
                                  const TokenCallback& onToken, bool& streamed)
{
  std::string reply;
  std::string held;
  streamed = false;
  llm_.chatStream(request, [&](const std::string& token, bool done) {
    if (done) {
      if (streamed)
        onToken("", true);
      return;
    }
    reply += token;
    if (streamed) {
      onToken(token, false);
      return;
    }
    held += token;
    if (mayOpenToolCall(held))
      return;
    streamed = true;
    onToken(held, false);
  });
  return reply;
}

namespace
{

// Handlers fall back to it when the model's arguments are incomplete.
std::string lastUserMessage(const std::vector<ChatMessage>& history)
{
  const auto found = std::find_if(
      history.rbegin(), history.rend(),
      [](const ChatMessage& message) { return message.role == "user"; });
  return found == history.rend() ? std::string() : found->content;
}

// The router picks the tool and the utterance is its only argument.
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
    default:
      return std::nullopt;
  }
  return call;
}

} // namespace

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
  if (!call || ToolRegistry::instance().find(call->name) == nullptr)
    return false;

  call->context = ctx.input.context;
  call->context.utterance = utterance;
  call->context.decided = true;

  const auto toolStart = std::chrono::steady_clock::now();
  const tools::ToolResult executed = executor_.execute(*call, ctx.input.role);
  ctx.output.toolMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - toolStart)
                          .count();
  if (!executed.ok) {
    LOG_WARN << "LfmAdapter: routed tool '" << call->name
             << "' failed: " << executed.output << "; falling back to the "
             << "tool loop";
    return false;
  }

  LOG_INFO << "LfmAdapter: router picked '" << call->name << "' ("
           << intent::toolIntentToString(decision.intent) << " score "
           << decision.score << (decision.fromRules ? ", rules" : ", model")
           << "): " << executed.output;
  ctx.output.executed.push_back(*call);
  ctx.output.hops = 1;
  ctx.history.push_back({.role = "tool", .content = executed.output});
  proseAnswer(ctx, ctx.input.toolTemperature);
  return true;
}

void LfmAdapter::proseAnswer(ToolHopContext ctx, float temperature)
{
  ChatRequest req;
  req.messages =
      hopMessages(ctx.history, ctx.input.systemPrompt, std::string());
  req.maxTokens =
      ctx.input.answerMaxTokens > 0 ? ctx.input.answerMaxTokens : 512;
  req.temperature = temperature;
  req.resetContext = false;
  req.stop = {};

  const auto genStart = std::chrono::steady_clock::now();
  std::string reply;
  if (ctx.onToken != nullptr) {
    const TokenCallback& onToken = *ctx.onToken;
    llm_.chatStream(req, [&reply, &onToken](const std::string& token,
                                            bool done) {
      reply += token;
      onToken(token, done);
    });
    ctx.output.emitted = true;
  }
  else {
    reply = llm_.chat(req);
  }
  ctx.output.generateMs +=
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - genStart)
          .count();
  ctx.output.reply = reply;
  ctx.history.push_back({.role = "assistant", .content = reply});
}

bool LfmAdapter::toolHops(ToolHopContext ctx, const std::string& declarations)
{
  const ToolChatInput& input = ctx.input;
  std::vector<ChatMessage>& history = ctx.history;
  ToolChatOutput& output = ctx.output;
  const TokenCallback* onToken = ctx.onToken;
  const int32_t cap =
      input.answerMaxTokens > 0 ? input.answerMaxTokens : 512;
  for (output.hops = 0; output.hops < input.maxHops; ++output.hops) {
    ChatRequest req;
    req.messages = hopMessages(history, input.systemPrompt, declarations);
    req.maxTokens = cap;
    req.temperature = input.toolTemperature;
    req.resetContext = output.hops == 0 && input.resetContext;
    if (!declarations.empty())
      req.stop = {kToolClose};

    bool streamed = false;
    const auto genStart = std::chrono::steady_clock::now();
    const std::string reply =
        onToken ? streamHop(req, *onToken, streamed) : llm_.chat(req);
    output.generateMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - genStart)
                             .count();
    history.push_back({.role = "assistant", .content = reply});

    // Once a byte is on the wire the hop is prose.
    const auto calls =
        streamed ? std::vector<tools::ToolCall>{} : parseToolCalls(reply);
    if (calls.empty()) {
      output.reply = reply;
      output.emitted = streamed;
      return true;
    }

    const std::string utterance = lastUserMessage(history);
    for (auto call : calls) {
      call.context = input.context;
      call.context.utterance = utterance;
      const auto toolStart = std::chrono::steady_clock::now();
      const auto executed = executor_.execute(call, input.role);
      output.toolMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - toolStart)
                           .count();
      if (executed.ok)
        LOG_INFO << "LfmAdapter: tool '" << call.name
                 << "' ok: " << executed.output;
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
  }
  return false;
}

ToolChatOutput LfmAdapter::chatWithTools(const ToolChatInput& input,
                                         std::vector<ChatMessage>& history)
{
  ToolChatOutput output;
  const ToolHopContext ctx{
      .input = input, .history = history, .output = output, .onToken = nullptr};

  if (routedTurn(ctx))
    return output;

  const std::string declarations = input.tools.empty()
                                       ? std::string()
                                       : buildToolDeclarations(input.tools);
  if (toolHops(ctx, declarations))
    return output;

  LOG_WARN << "LfmAdapter: tool loop exhausted after " << input.maxHops
           << " hops";
  proseAnswer(ctx, input.temperature);
  return output;
}

ToolChatOutput LfmAdapter::chatWithToolsStream(
    const ToolChatInput& input, std::vector<ChatMessage>& history,
    const TokenCallback& onToken)
{
  ToolChatOutput output;
  const ToolHopContext ctx{.input = input,
                           .history = history,
                           .output = output,
                           .onToken = &onToken};

  if (routedTurn(ctx))
    return output;

  const std::string declarations = input.tools.empty()
                                       ? std::string()
                                       : buildToolDeclarations(input.tools);
  if (toolHops(ctx, declarations)) {
    // The final hop held its tokens back to guard the call boundary.
    if (!output.emitted) {
      onToken(output.reply, false);
      onToken("", true);
    }
    return output;
  }

  LOG_WARN << "LfmAdapter: tool loop exhausted after " << input.maxHops
           << " hops";
  proseAnswer(ctx, input.temperature);
  return output;
}
