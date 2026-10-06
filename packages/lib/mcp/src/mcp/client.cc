#include "client.hxx"

#include <utility>

namespace argus::mcp
{

namespace
{
template <class T>
Response<T> failed(Failure::Kind kind, const RpcError& error)
{
  return {.value = std::nullopt, .failure = {.kind = kind, .error = error}};
}

template <class T>
Response<T> malformed(const std::string& message)
{
  return failed<T>(Failure::Kind::Malformed,
                   {.code = codeOf(ErrorCode::InvalidRequest), .message = message, .data = Json::Value()});
}

template <class T, class U>
Response<T> relayed(const Response<U>& other)
{
  return {.value = std::nullopt, .failure = other.failure};
}

std::string textAt(const Json::Value& json, const char* key)
{
  return json[key].isString() ? json[key].asString() : std::string();
}
}

McpClient::McpClient(std::shared_ptr<Transport> transport, ClientIdentity identity)
    : transport_(std::move(transport)), identity_(std::move(identity))
{
}

Response<Json::Value> McpClient::request(const std::string& method, Json::Value params) const
{
  Json::Value info(Json::objectValue);
  info["name"] = identity_.name;
  info["version"] = identity_.version;
  const Json::Value meta = requestMeta(info);
  for (const auto& key : meta.getMemberNames())
    params["_meta"][key] = meta[key];
  const Json::Value id(nextId_.fetch_add(1));
  std::optional<std::string> answer;
  if (transport_)
    answer = transport_->exchange(requestFrame({.hasId = true, .id = id, .method = method, .params = std::move(params)}));
  if (!answer)
    return failed<Json::Value>(Failure::Kind::Transport,
                               {.code = codeOf(ErrorCode::InternalError),
                                .message = "The tool server did not answer",
                                .data = Json::Value()});
  auto response = parseResponse(*answer);
  if (!response)
    return malformed<Json::Value>("The tool server answered with an invalid frame");
  if (response->id != id)
    return malformed<Json::Value>("The tool server answered a different request");
  if (response->error)
    return failed<Json::Value>(Failure::Kind::Protocol, *response->error);
  if (!response->result)
    return malformed<Json::Value>("The tool server answered without a result");
  return Response<Json::Value>{.value = std::move(response->result), .failure = {}};
}

Response<ServerDescription> McpClient::discover() const
{
  const auto answer = request("server/discover", Json::Value(Json::objectValue));
  if (!answer.value)
    return relayed<ServerDescription>(answer);
  const Json::Value& result = *answer.value;
  ServerDescription description;
  for (const auto& version : result["supportedVersions"])
    if (version.isString())
      description.supportedVersions.push_back(version.asString());
  const Json::Value& info = result["_meta"][std::string(kServerInfoKey)];
  description.name = textAt(info, "name");
  description.version = textAt(info, "version");
  description.instructions = textAt(result, "instructions");
  description.tools = result["capabilities"].isMember("tools");
  return Response<ServerDescription>{.value = std::move(description), .failure = {}};
}

Response<ToolList> McpClient::listTools() const
{
  ToolList list;
  std::string cursor;
  for (int page = 0; page < kMaxPages; ++page) {
    Json::Value params(Json::objectValue);
    if (!cursor.empty())
      params["cursor"] = cursor;
    const auto answer = request("tools/list", std::move(params));
    if (!answer.value)
      return relayed<ToolList>(answer);
    const Json::Value& result = *answer.value;
    if (!result["tools"].isArray())
      return malformed<ToolList>("The tool list is not an array");
    for (const auto& entry : result["tools"])
      if (auto spec = toolSpecFrom(entry))
        list.tools.push_back(std::move(*spec));
    list.ttlMs = result["ttlMs"].isIntegral() ? result["ttlMs"].asInt64() : 0;
    cursor = textAt(result, "nextCursor");
    if (cursor.empty())
      return Response<ToolList>{.value = std::move(list), .failure = {}};
  }
  return malformed<ToolList>("The tool list has too many pages");
}

Response<ToolOutcome> McpClient::callTool(const ToolInvocation& invocation) const
{
  Json::Value params(Json::objectValue);
  params["name"] = invocation.name;
  params["arguments"] = invocation.arguments;
  params["_meta"][std::string(kContextKey)] = toJson(invocation.caller);
  const auto answer = request("tools/call", std::move(params));
  if (!answer.value)
    return relayed<ToolOutcome>(answer);
  auto outcome = toolOutcomeFrom(*answer.value);
  if (!outcome)
    return malformed<ToolOutcome>("The tool result is malformed");
  return Response<ToolOutcome>{.value = std::move(outcome), .failure = {}};
}

}
