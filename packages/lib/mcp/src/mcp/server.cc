#include "server.hxx"

#include <mcp/schema.hxx>

#include <trantor/utils/Logger.h>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace argus::mcp
{

namespace
{
constexpr int64_t kListTtlMs = 30000;

Json::Value supportedVersions()
{
  Json::Value versions(Json::arrayValue);
  versions.append(std::string(kProtocolVersion));
  return versions;
}

RpcError invalidParams(std::string message)
{
  return {.code = codeOf(ErrorCode::InvalidParams), .message = std::move(message), .data = Json::Value()};
}

std::optional<RpcError> checkMeta(const Json::Value& params)
{
  const Json::Value& meta = params["_meta"];
  if (!meta.isObject() || !meta[std::string(kVersionKey)].isString() ||
      !meta[std::string(kClientCapabilitiesKey)].isObject())
    return invalidParams("_meta must carry the protocol version and the client capabilities");
  const std::string requested = meta[std::string(kVersionKey)].asString();
  if (requested == kProtocolVersion)
    return std::nullopt;
  Json::Value data(Json::objectValue);
  data["supported"] = supportedVersions();
  data["requested"] = requested;
  return RpcError{.code = codeOf(ErrorCode::UnsupportedProtocolVersion),
                  .message = "Unsupported protocol version",
                  .data = data};
}

ToolOutcome failure(const Refusal& refusal)
{
  ToolOutcome outcome;
  outcome.isError = true;
  outcome.text = refusal.message;
  outcome.structured = Json::Value(Json::objectValue);
  outcome.structured["code"] = refusal.code;
  return outcome;
}

struct Rendezvous
{
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<std::string> frame;
};
}

McpServer::McpServer(ServerIdentity identity) : identity_(std::move(identity)) {}

void McpServer::add(ToolSpec spec, Handler handler)
{
  if (!validToolName(spec.name))
    throw std::invalid_argument("invalid tool name: " + spec.name);
  if (entries_.contains(spec.name))
    throw std::invalid_argument("duplicate tool: " + spec.name);
  if (!handler)
    throw std::invalid_argument("tool without a handler: " + spec.name);
  if (const auto reason = schema::unsupported(spec.inputSchema))
    throw std::invalid_argument("tool " + spec.name + ": " + *reason);
  const std::string name = spec.name;
  entries_.emplace(name, Entry{.spec = std::move(spec), .handler = std::move(handler)});
}

void McpServer::addSync(ToolSpec spec, SyncHandler handler)
{
  if (!handler)
    throw std::invalid_argument("tool without a handler: " + spec.name);
  add(std::move(spec), [handler = std::move(handler)](const ToolInvocation& invocation, const Reply& reply) {
    reply(handler(invocation));
  });
}

void McpServer::setGate(Gate gate)
{
  gate_ = std::move(gate);
}

std::vector<ToolSpec> McpServer::tools() const
{
  std::vector<ToolSpec> specs;
  specs.reserve(entries_.size());
  for (const auto& [name, entry] : entries_)
    specs.push_back(entry.spec);
  return specs;
}

const ToolSpec* McpServer::find(std::string_view name) const
{
  const auto found = entries_.find(name);
  return found == entries_.end() ? nullptr : &found->second.spec;
}

Json::Value McpServer::serverMeta() const
{
  Json::Value info(Json::objectValue);
  info["name"] = identity_.name;
  info["version"] = identity_.version;
  Json::Value meta(Json::objectValue);
  meta[std::string(kServerInfoKey)] = std::move(info);
  return meta;
}

void McpServer::handle(std::string_view frame, const Done& done) const
{
  const ParsedRequest parsed = parseRequest(frame);
  if (const auto* rejection = std::get_if<RpcRejection>(&parsed)) {
    done(errorFrame(rejection->id, rejection->error));
    return;
  }
  const auto& request = std::get<RpcRequest>(parsed);
  if (request.notification()) {
    done(std::string());
    return;
  }
  dispatch({.request = request, .done = done});
}

void McpServer::dispatch(const Exchange& exchange) const
{
  const RpcRequest& request = exchange.request;
  if (request.method == "initialize") {
    Json::Value data(Json::objectValue);
    data["supported"] = supportedVersions();
    exchange.done(errorFrame(request.id, {.code = codeOf(ErrorCode::MethodNotFound),
                                          .message = "initialize is not part of protocol " +
                                                     std::string(kProtocolVersion),
                                          .data = data}));
    return;
  }
  const bool known = request.method == "server/discover" || request.method == "tools/list" ||
                     request.method == "tools/call";
  if (!known) {
    exchange.done(errorFrame(request.id, {.code = codeOf(ErrorCode::MethodNotFound),
                                          .message = "Method not found: " + request.method,
                                          .data = Json::Value()}));
    return;
  }
  if (const auto refused = checkMeta(request.params)) {
    exchange.done(errorFrame(request.id, *refused));
    return;
  }
  if (request.method == "server/discover")
    discover(exchange);
  else if (request.method == "tools/list")
    list(exchange);
  else
    call(exchange);
}

void McpServer::discover(const Exchange& exchange) const
{
  Json::Value result(Json::objectValue);
  result["resultType"] = "complete";
  result["supportedVersions"] = supportedVersions();
  Json::Value tools(Json::objectValue);
  tools["listChanged"] = false;
  Json::Value capabilities(Json::objectValue);
  capabilities["tools"] = std::move(tools);
  result["capabilities"] = std::move(capabilities);
  result["_meta"] = serverMeta();
  if (!identity_.instructions.empty())
    result["instructions"] = identity_.instructions;
  result["ttlMs"] = kListTtlMs;
  result["cacheScope"] = "public";
  exchange.done(resultFrame({.id = exchange.request.id, .result = std::move(result)}));
}

void McpServer::list(const Exchange& exchange) const
{
  Json::Value result(Json::objectValue);
  result["resultType"] = "complete";
  Json::Value listed(Json::arrayValue);
  for (const auto& [name, entry] : entries_)
    listed.append(toJson(entry.spec));
  result["tools"] = std::move(listed);
  result["_meta"] = serverMeta();
  result["ttlMs"] = kListTtlMs;
  result["cacheScope"] = "public";
  exchange.done(resultFrame({.id = exchange.request.id, .result = std::move(result)}));
}

void McpServer::call(const Exchange& exchange) const
{
  const Json::Value& params = exchange.request.params;
  if (!params["name"].isString() || params["name"].asString().empty()) {
    exchange.done(errorFrame(exchange.request.id, invalidParams("name is required")));
    return;
  }
  if (params.isMember("arguments") && !params["arguments"].isObject()) {
    exchange.done(errorFrame(exchange.request.id, invalidParams("arguments must be an object")));
    return;
  }
  const auto found = entries_.find(params["name"].asString());
  if (found == entries_.end()) {
    exchange.done(errorFrame(exchange.request.id, invalidParams("Unknown tool: " + params["name"].asString())));
    return;
  }
  const Entry& entry = found->second;
  ToolInvocation invocation{.name = entry.spec.name,
                            .arguments = params.isMember("arguments") ? params["arguments"]
                                                                      : Json::Value(Json::objectValue),
                            .caller = callerContextFrom(params["_meta"][std::string(kContextKey)])};
  if (const auto invalid = schema::violation(entry.spec.inputSchema, invocation.arguments)) {
    exchange.done(resultFrame({.id = exchange.request.id,
                               .result = toJson(failure({.code = "invalid_arguments", .message = *invalid}))}));
    return;
  }
  if (gate_) {
    if (const auto refusal = gate_(invocation, entry.spec)) {
      exchange.done(resultFrame({.id = exchange.request.id, .result = toJson(failure(*refusal))}));
      return;
    }
  }
  run(entry, invocation, exchange);
}

void McpServer::run(const Entry& entry, const ToolInvocation& invocation, const Exchange& exchange) const
{
  const auto answered = std::make_shared<std::atomic<bool>>(false);
  const Json::Value id = exchange.request.id;
  const Done done = exchange.done;
  const Json::Value meta = serverMeta();
  const Reply reply = [answered, id, done, meta](const ToolOutcome& outcome) {
    if (answered->exchange(true))
      return;
    Json::Value result = toJson(outcome);
    result["_meta"][std::string(kServerInfoKey)] = meta[std::string(kServerInfoKey)];
    done(resultFrame({.id = id, .result = std::move(result)}));
  };
  try {
    entry.handler(invocation, reply);
  }
  catch (const std::exception& error) {
    LOG_WARN << "mcp: tool " << entry.spec.name << " failed: " << error.what();
    reply(failure({.code = "internal_error", .message = "The tool failed: " + entry.spec.name}));
  }
}

std::string McpServer::handleBlocking(std::string_view frame, std::chrono::milliseconds timeout) const
{
  const auto rendezvous = std::make_shared<Rendezvous>();
  handle(frame, [rendezvous](std::string response) {
    {
      const std::scoped_lock lock(rendezvous->mutex);
      rendezvous->frame = std::move(response);
    }
    rendezvous->ready.notify_all();
  });
  std::unique_lock lock(rendezvous->mutex);
  if (!rendezvous->ready.wait_for(lock, timeout, [&rendezvous] { return rendezvous->frame.has_value(); }))
    return errorFrame(Json::Value(),
                      {.code = codeOf(ErrorCode::InternalError), .message = "The tool did not answer", .data = Json::Value()});
  return rendezvous->frame.value_or(std::string());
}

}
