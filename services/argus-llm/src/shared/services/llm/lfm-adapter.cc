#include "lfm-adapter.hxx"

#include <cstring>
#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>
#include <sstream>

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
        std::string key = body.substr(argPos, eq - argPos);
        while (!key.empty() && key.front() == ' ')
          key.erase(key.begin());
        while (!key.empty() && key.back() == ' ')
          key.pop_back();
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

ToolChatOutput LfmAdapter::chatWithTools(const ToolChatInput& input,
                                         std::vector<ChatMessage>& history)
{
  ToolChatOutput output;
  std::string system = input.systemPrompt;
  if (!input.tools.empty())
    system += "\nList of tools: " + buildToolDeclarations(input.tools);

  for (output.hops = 0; output.hops < input.maxHops; ++output.hops) {
    ChatRequest req;
    req.messages = history;
    req.messages.insert(req.messages.begin(),
                        ChatMessage{.role = "system", .content = system});
    req.maxTokens = 512;
    req.temperature = input.temperature;
    req.resetContext = false;

    const std::string reply = llm_.chat(req);
    history.push_back({.role = "assistant", .content = reply});

    const auto calls = parseToolCalls(reply);
    if (calls.empty()) {
      output.reply = reply;
      return output;
    }

    for (auto call : calls) {
      call.context = input.context;
      ToolExecutor executor(ToolRegistry::instance());
      const auto executed = executor.execute(call, input.role);
      output.executed.push_back(call);
      history.push_back({.role = "tool", .content = executed.output});
    }
  }
  LOG_WARN << "LfmAdapter: tool loop exhausted after " << input.maxHops
           << " hops";
  output.reply = history.back().content;
  return output;
}
