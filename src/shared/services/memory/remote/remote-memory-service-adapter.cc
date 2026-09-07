#include "remote-memory-service-adapter.hxx"

#include <shared/services/memory/memory-tool-descriptors.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <trantor/utils/Logger.h>

namespace
{

constexpr const char* kRememberPath = "/memory/v1/remember";
constexpr const char* kRecallPath = "/memory/v1/recall";
constexpr const char* kForgetPath = "/memory/v1/forget";
constexpr const char* kProcedureRunPath = "/memory/v1/procedure-run";
constexpr const char* kCapturePath = "/memory/v1/capture";
constexpr const char* kCompactPath = "/memory/v1/compact";
constexpr const char* kDurableTranscriptPath =
    "/memory/v1/durable-transcript";

Json::Value contextBody(const tools::ToolContext& context)
{
  Json::Value contextJson(Json::objectValue);
  contextJson["user_id"] = static_cast<Json::Int64>(context.userId);
  contextJson["lang"] = context.lang;
  contextJson["session_id"] = context.sessionId;
  return contextJson;
}

tools::ToolResult toToolResult(const std::string& name, const Json::Value& info)
{
  tools::ToolResult result;
  result.tool = name;
  result.ok = info.get("ok", false).asBool();
  result.output = info.get("output", "").asString();
  result.data = info.get("data", Json::Value(Json::objectValue));
  return result;
}

} // namespace

bool RemoteMemoryServiceAdapter::initialize()
{
  config_ = MemoryRemoteConfig::resolve();
  if (!config_.enabled())
    return false;

  client_ = std::make_unique<MemoryHttpClient>(config_.url, config_.timeoutMs);

  // Same descriptors as the in-process registration (shared metadata),
  // wire-forwarding handlers: the tool loop cannot tell the substrates
  // apart.
  auto& registry = ToolRegistry::instance();
  for (tools::ToolDescriptor descriptor : memoryToolDescriptors()) {
    const std::string path =
        descriptor.name == "memory.remember"  ? kRememberPath
        : descriptor.name == "memory.recall"  ? kRecallPath
        : descriptor.name == "memory.forget"  ? kForgetPath
                                              : kProcedureRunPath;
    descriptor.handler = [this, path, name = descriptor.name](
                             const tools::ToolCall& call) {
      Json::Value body = call.arguments;
      if (body.isNull())
        body = Json::Value(Json::objectValue);
      body["context"] = contextBody(call.context);
      try {
        return toToolResult(name, client_->call(path, body));
      }
      catch (const std::exception& error) {
        // The in-process substrate returns ok=false, never throws; the tool
        // loop must not distinguish the substrates on an argus-memory outage.
        LOG_WARN << "RemoteMemoryServiceAdapter: " << name << " failed ("
                 << error.what() << ")";
        tools::ToolResult degraded;
        degraded.tool = name;
        degraded.output = "la memoria no esta disponible ahora";
        return degraded;
      }
    };
    registry.registerTool(std::move(descriptor));
  }
  return true;
}

bool RemoteMemoryServiceAdapter::isLoaded() const
{
  return client_ != nullptr;
}

void RemoteMemoryServiceAdapter::shutdown()
{
  client_.reset();
}

Json::Value RemoteMemoryServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = client_ != nullptr;
  value["remote"] = config_.url;
  return value;
}

CaptureResult RemoteMemoryServiceAdapter::captureExplicit(
    const CaptureInput& input)
{
  Json::Value body(Json::objectValue);
  body["text"] = input.text;
  body["lang"] = input.lang;
  body["user_id"] = static_cast<Json::Int64>(input.userId);

  CaptureResult result;
  Json::Value info;
  try {
    info = client_->call(kCapturePath, body);
  }
  catch (const std::exception& error) {
    // The in-process capture degrades to Rejected on a store failure; the
    // voice turn must survive argus-memory being down the same way.
    LOG_WARN << "RemoteMemoryServiceAdapter: capture failed (" << error.what()
             << ")";
    return result;
  }

  const std::string outcome = info.get("outcome", "rejected").asString();
  if (outcome == "stored")
    result.outcome = CaptureOutcome::Stored;
  else if (outcome == "deferred")
    result.outcome = CaptureOutcome::Deferred;
  result.factId = info.get("fact_id", 0).asInt64();
  return result;
}

void RemoteMemoryServiceAdapter::enqueueCompaction(
    int64_t userId, const std::string& transcript, const std::string& lang)
{
  Json::Value body(Json::objectValue);
  body["user_id"] = static_cast<Json::Int64>(userId);
  body["transcript"] = transcript;
  body["lang"] = lang;
  try {
    client_->call(kCompactPath, body);
  }
  catch (const std::exception& error) {
    LOG_WARN << "RemoteMemoryServiceAdapter: compaction enqueue failed ("
             << error.what() << ")";
  }
}

std::string RemoteMemoryServiceAdapter::durableTranscript(
    const std::string& transcript, const std::string& lang)
{
  Json::Value body(Json::objectValue);
  body["transcript"] = transcript;
  body["lang"] = lang;
  try {
    return client_->call(kDurableTranscriptPath, body)
        .get("text", "")
        .asString();
  }
  catch (const std::exception& error) {
    LOG_WARN << "RemoteMemoryServiceAdapter: durable transcript failed ("
             << error.what() << ")";
    return {};
  }
}
