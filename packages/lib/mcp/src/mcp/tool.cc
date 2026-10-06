#include "tool.hxx"

#include <algorithm>
#include <utility>

namespace argus::mcp
{

namespace
{
constexpr size_t kMaxNameLength = 128;

bool nameCharacter(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
}

std::string stringAt(const Json::Value& json, const char* key)
{
  return json[key].isString() ? json[key].asString() : std::string();
}

bool flagAt(const Json::Value& json, const char* key)
{
  return json[key].isBool() && json[key].asBool();
}

Json::Value annotationsJson(const ToolSpec& spec)
{
  Json::Value out(Json::objectValue);
  if (!spec.title.empty())
    out["title"] = spec.title;
  out["readOnlyHint"] = spec.annotations.readOnly;
  out["destructiveHint"] = spec.annotations.destructive;
  out["idempotentHint"] = spec.annotations.idempotent;
  out["openWorldHint"] = spec.annotations.openWorld;
  return out;
}
}

bool validToolName(std::string_view name)
{
  return !name.empty() && name.size() <= kMaxNameLength && std::ranges::all_of(name, nameCharacter);
}

Json::Value toJson(const ToolSpec& spec)
{
  Json::Value out(Json::objectValue);
  out["name"] = spec.name;
  if (!spec.title.empty())
    out["title"] = spec.title;
  out["description"] = spec.description;
  out["inputSchema"] = spec.inputSchema;
  out["annotations"] = annotationsJson(spec);
  Json::Value meta(Json::objectValue);
  if (!spec.module.empty())
    meta[std::string(kModuleKey)] = spec.module;
  if (!spec.capability.empty())
    meta[std::string(kCapabilityKey)] = spec.capability;
  if (!meta.empty())
    out["_meta"] = std::move(meta);
  return out;
}

std::optional<ToolSpec> toolSpecFrom(const Json::Value& json)
{
  if (!json.isObject() || !json["name"].isString() || !validToolName(json["name"].asString()) ||
      !json["inputSchema"].isObject())
    return std::nullopt;
  ToolSpec spec;
  spec.name = json["name"].asString();
  spec.title = stringAt(json, "title");
  spec.description = stringAt(json, "description");
  spec.inputSchema = json["inputSchema"];
  const Json::Value& annotations = json["annotations"];
  spec.annotations = {.readOnly = flagAt(annotations, "readOnlyHint"),
                      .destructive = flagAt(annotations, "destructiveHint"),
                      .idempotent = flagAt(annotations, "idempotentHint"),
                      .openWorld = flagAt(annotations, "openWorldHint")};
  if (spec.title.empty())
    spec.title = stringAt(annotations, "title");
  const Json::Value& meta = json["_meta"];
  spec.module = stringAt(meta, std::string(kModuleKey).c_str());
  spec.capability = stringAt(meta, std::string(kCapabilityKey).c_str());
  return spec;
}

Json::Value toJson(const CallerContext& caller)
{
  Json::Value out(Json::objectValue);
  out["userId"] = caller.userId;
  out["role"] = caller.role;
  out["lang"] = caller.lang;
  out["sessionId"] = caller.sessionId;
  out["utterance"] = caller.utterance;
  out["decided"] = caller.decided;
  return out;
}

CallerContext callerContextFrom(const Json::Value& json)
{
  CallerContext caller;
  if (!json.isObject())
    return caller;
  caller.userId = json["userId"].isIntegral() ? json["userId"].asInt64() : 0;
  caller.role = stringAt(json, "role");
  caller.lang = stringAt(json, "lang");
  caller.sessionId = stringAt(json, "sessionId");
  caller.utterance = stringAt(json, "utterance");
  caller.decided = flagAt(json, "decided");
  return caller;
}

Json::Value toJson(const ToolOutcome& outcome)
{
  Json::Value out(Json::objectValue);
  out["resultType"] = "complete";
  Json::Value block(Json::objectValue);
  block["type"] = "text";
  block["text"] = outcome.text;
  Json::Value content(Json::arrayValue);
  content.append(std::move(block));
  out["content"] = std::move(content);
  if (!outcome.structured.isNull())
    out["structuredContent"] = outcome.structured;
  out["isError"] = outcome.isError;
  if (outcome.appAction) {
    Json::Value action(Json::objectValue);
    action["name"] = outcome.appAction->name;
    action["arguments"] = outcome.appAction->arguments;
    Json::Value meta(Json::objectValue);
    meta[std::string(kAppActionKey)] = std::move(action);
    out["_meta"] = std::move(meta);
  }
  return out;
}

std::optional<ToolOutcome> toolOutcomeFrom(const Json::Value& result)
{
  if (!result.isObject() || !result["content"].isArray() ||
      (result.isMember("resultType") && result["resultType"].asString() != "complete"))
    return std::nullopt;
  ToolOutcome outcome;
  outcome.isError = flagAt(result, "isError");
  for (const auto& block : result["content"]) {
    if (!block.isObject() || block["type"].asString() != "text" || !block["text"].isString())
      continue;
    if (!outcome.text.empty())
      outcome.text += '\n';
    outcome.text += block["text"].asString();
  }
  outcome.structured = result["structuredContent"];
  const Json::Value& action = result["_meta"][std::string(kAppActionKey)];
  if (action.isObject() && action["name"].isString())
    outcome.appAction = AppAction{.name = action["name"].asString(),
                                  .arguments = action["arguments"].isObject() ? action["arguments"]
                                                                              : Json::Value(Json::objectValue)};
  return outcome;
}

Json::Value requestMeta(const Json::Value& clientInfo)
{
  Json::Value meta(Json::objectValue);
  meta[std::string(kVersionKey)] = std::string(kProtocolVersion);
  meta[std::string(kClientInfoKey)] = clientInfo;
  meta[std::string(kClientCapabilitiesKey)] = Json::Value(Json::objectValue);
  return meta;
}

}
