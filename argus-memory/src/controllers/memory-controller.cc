#include "memory-controller.hxx"

#include <shared/wrapper/api-response/api-response.hxx>

#include <drogon/drogon.h>

#include <chrono>
#include <string>

namespace
{

drogon::HttpResponsePtr badRequest()
{
  return ApiResponse::error(400, "BAD_REQUEST", "Body must be a JSON object");
}

drogon::HttpResponsePtr notLoaded()
{
  return ApiResponse::error(503, "MEMORY_NOT_LOADED",
                            "Memory stack is not loaded");
}

drogon::HttpResponsePtr toolResult(const tools::ToolResult& result)
{
  Json::Value info(Json::objectValue);
  info["ok"] = result.ok;
  info["output"] = result.output;
  info["data"] = result.data;
  return ApiResponse::ok(info);
}

} // namespace

MemoryController::MemoryController()
    : chat_([] {
        const LlmRemoteConfig config = LlmRemoteConfig::resolve();
        return std::make_unique<WireMemoryChat>(config.url, config.timeoutMs);
      }()),
      service_(VecDb::instance(), *chat_)
{
}

void MemoryController::initStack()
{
  service_.init({.deferStore = true});
  service_.registerTools(registry_);
}

void MemoryController::shutdownStack()
{
  service_.shutdown();
}

bool MemoryController::isStackLoaded() const
{
  return service_.isLoaded();
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::remember(drogon::HttpRequestPtr req)
{
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = RememberBody::fromJson(*req->getJsonObject());

  Json::Value args(Json::objectValue);
  args["subject"] = body.subject;
  args["predicate"] = body.predicate;
  args["value"] = body.value;
  args["type"] = body.type;
  if (body.confidence)
    args["confidence"] = *body.confidence;
  co_return co_await runTool("memory.remember", args, body.context);
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::recall(drogon::HttpRequestPtr req)
{
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = RecallBody::fromJson(*req->getJsonObject());

  Json::Value args(Json::objectValue);
  args["query"] = body.query;
  co_return co_await runTool("memory.recall", args, body.context);
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::forget(drogon::HttpRequestPtr req)
{
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = ForgetBody::fromJson(*req->getJsonObject());

  Json::Value args(Json::objectValue);
  args["fact_id"] = static_cast<Json::Int64>(body.factId);
  co_return co_await runTool("memory.forget", args,
                             MemoryToolContext{});
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::procedureRun(drogon::HttpRequestPtr req)
{
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = ProcedureBody::fromJson(*req->getJsonObject());

  Json::Value args(Json::objectValue);
  args["goal"] = body.goal;
  co_return co_await runTool("procedure.run", args, MemoryToolContext{});
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::capture(drogon::HttpRequestPtr req)
{
  if (!isStackLoaded())
    co_return notLoaded();
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = CaptureBody::fromJson(*req->getJsonObject());

  const CaptureResult result = service_.captureExplicit(
      {.userId = body.userId, .lang = body.lang, .text = body.text});
  Json::Value info(Json::objectValue);
  info["outcome"] = result.outcome == CaptureOutcome::Stored  ? "stored"
                    : result.outcome == CaptureOutcome::Deferred
                        ? "deferred"
                        : "rejected";
  info["fact_id"] = static_cast<Json::Int64>(result.factId);
  co_return ApiResponse::ok(info);
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::compact(drogon::HttpRequestPtr req)
{
  if (!isStackLoaded())
    co_return notLoaded();
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = CompactBody::fromJson(*req->getJsonObject());

  service_.enqueueCompaction(body.userId, body.transcript, body.lang);
  Json::Value info(Json::objectValue);
  info["queued"] = true;
  co_return ApiResponse::ok(info);
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::durableTranscript(drogon::HttpRequestPtr req)
{
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();
  const auto body = DurableTranscriptBody::fromJson(*req->getJsonObject());

  Json::Value info(Json::objectValue);
  info["text"] = service_.durableTranscript(body.transcript, body.lang);
  co_return ApiResponse::ok(info);
}

drogon::Task<drogon::HttpResponsePtr>
MemoryController::runTool(const std::string& name,
                          const Json::Value& arguments,
                          const MemoryToolContext& context)
{
  if (!isStackLoaded())
    co_return notLoaded();

  const tools::ToolDescriptor* descriptor = registry_.find(name);
  if (!descriptor)
    co_return ApiResponse::error(404, "TOOL_NOT_FOUND",
                                 "No tool registered as " + name);

  tools::ToolCall call;
  call.name = name;
  call.arguments = arguments;
  call.context = {.userId = context.userId,
                  .lang = context.lang,
                  .sessionId = context.sessionId,
                  .channel = "tool_call"};

  const auto t0 = std::chrono::steady_clock::now();
  const tools::ToolResult result = descriptor->handler(call);
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "Memory wire: " << name << " ok=" << result.ok
           << " ms=" << static_cast<int>(ms);
  co_return toolResult(result);
}
