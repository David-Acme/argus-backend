#include "llm-controller.hxx"

#include <llm/chat-dto.hxx>
#include <shared/services/llm/lfm-adapter.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <shared/wrapper/api-response/api-response.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

#include <drogon/drogon.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

drogon::HttpResponsePtr notLoaded()
{
  return ApiResponse::error(503, "LLM_NOT_LOADED",
                            "LLM engine is not loaded");
}

drogon::HttpResponsePtr badRequest()
{
  return ApiResponse::error(400, "BAD_REQUEST", "Body must be a JSON object");
}

// The bench-exact framing from f8-b1 — Spanish, the tool named with its
// trigger, a short answer otherwise. Measured on the live wire: 8/20
// memory_save fires against 1/20 for an English generic cut and 2/20
// (wrong-tool) for a five-sentence variant; the f8-b4 record in
// docs/CONTEXT.md carries the numbers.
constexpr const char* kToolPolicy =
    "Eres Argus. Si el usuario pide guardar o recordar algo, usa "
    "memory.remember. Si no, responde brevemente.";

// The hosted memory stack registers its tools at boot; a process that
// registered nothing (llm-wire-test, the bench) keeps the direct paths.
std::vector<const tools::ToolDescriptor*> registeredTools()
{
  std::vector<const tools::ToolDescriptor*> out;
  for (const auto& name : ToolRegistry::instance().names()) {
    if (const auto* descriptor = ToolRegistry::instance().find(name))
      out.push_back(descriptor);
  }
  return out;
}

std::vector<ChatMessage> toChatMessages(const ChatCompletionDto& body)
{
  std::vector<ChatMessage> messages;
  messages.reserve(body.messages.size());
  for (const auto& message : body.messages)
    messages.push_back({.role = message.role, .content = message.content});
  return messages;
}

// The loop mirrors the direct path's wire semantics: the caller's
// max_tokens (or the service default) caps every generation, and
// reset_context resets before hop one.
ToolChatInput toolLoopInput(const ChatCompletionDto& body,
                            const std::vector<const tools::ToolDescriptor*>& tools,
                            int32_t defaultMaxTokens)
{
  ToolChatInput input;
  input.systemPrompt = kToolPolicy;
  input.tools = tools;
  input.role = UserRole::Resident;
  input.context = tools::ToolContext{};
  input.maxHops = 3;
  input.temperature = body.temperature ? *body.temperature : -1.0F;
  input.resetContext = body.resetContext;
  input.answerMaxTokens = body.maxTokens ? *body.maxTokens : defaultMaxTokens;
  return input;
}

// Producer state for the chunked stream leg.
struct ChatStreamJob
{
  LlmController* owner{nullptr};
  ChatRequest request;
  ToolChatInput loop;
  std::vector<ChatMessage> history;
  std::unique_ptr<drogon::ResponseStream> stream;
  size_t tokenCount{0};
  size_t charCount{0};
};

std::string sentinelLine(const LlmPrefillStats& stats)
{
  Json::Value sentinel(Json::objectValue);
  sentinel["done"] = true;
  sentinel["prompt_tokens"] = stats.promptTokens;
  sentinel["reused_tokens"] = stats.reusedTokens;
  sentinel["decoded_tokens"] = stats.decodedTokens;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, sentinel) + "\n";
}

void runStreamJob(const std::shared_ptr<ChatStreamJob>& job)
{
  const auto t0 = std::chrono::steady_clock::now();
  auto& service = job->owner->service();
  const TokenCallback send = [&job,
                              &service](const std::string& token, bool done) {
    if (!job->stream)
      return;
    if (!done) {
      if (!job->stream->send(token)) {
        job->stream.reset();
        return;
      }
      ++job->tokenCount;
      job->charCount += token.size();
      return;
    }
    const std::string line = "\n" + sentinelLine(service.lastPrefillStats());
    if (!job->stream->send(line))
      job->stream.reset();
  };
  try {
    if (job->loop.tools.empty()) {
      service.chatStream(job->request, send);
    }
    else {
      const ToolChatOutput output =
          LfmAdapter(service).chatWithToolsStream(job->loop, job->history, send);
      LOG_INFO << "LLM stream loop: hops=" << output.hops
               << " tools=" << output.executed.size()
               << " gen_ms=" << output.generateMs
               << " tool_ms=" << output.toolMs;
    }
  }
  catch (const std::exception& e) {
    LOG_ERROR << "LLM stream generation failed: " << e.what();
  }
  if (job->stream) {
    job->stream->close();
    job->stream.reset();
  }
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "LLM stream: tokens=" << job->tokenCount << " chars="
           << job->charCount << " ms=" << static_cast<int>(ms);
}

} // namespace

drogon::Task<drogon::HttpResponsePtr>
LlmController::chat(drogon::HttpRequestPtr req)
{
  if (!service_.isLoaded())
    co_return notLoaded();
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();

  const auto body = ChatCompletionDto::fromJson(*req->getJsonObject());

  const auto t0 = std::chrono::steady_clock::now();
  std::string text;
  int hops = 0;
  size_t toolCalls = 0;
  int64_t generateMs = 0;
  int64_t toolMs = 0;
  const auto tools = registeredTools();
  if (tools.empty()) {
    text = co_await service_.chatAsync(body.request());
  }
  else {
    const auto output = co_await BlockingTask<ToolChatOutput>([this,
                                                                input =
                                                                    toolLoopInput(
                                                                        body,
                                                                        tools,
                                                                        service_
                                                                            .defaultMaxTokens()),
                                                                history =
                                                                    toChatMessages(
                                                                        body)]()
                                                                   mutable {
      return LfmAdapter(service_).chatWithTools(input, history);
    });
    text = output.reply;
    hops = output.hops;
    toolCalls = output.executed.size();
    generateMs = output.generateMs;
    toolMs = output.toolMs;
  }
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  if (hops == 0)
    LOG_INFO << "LLM chat: messages=" << body.messages.size()
             << " chars=" << text.size() << " ms=" << static_cast<int>(ms);
  else
    LOG_INFO << "LLM chat loop: hops=" << hops << " tools=" << toolCalls
             << " gen_ms=" << generateMs << " tool_ms=" << toolMs
             << " messages=" << body.messages.size()
             << " chars=" << text.size() << " ms=" << static_cast<int>(ms);

  Json::Value info(Json::objectValue);
  info["text"] = std::move(text);
  co_return ApiResponse::ok(info);
}

drogon::Task<drogon::HttpResponsePtr>
LlmController::chatStream(drogon::HttpRequestPtr req)
{
  if (!service_.isLoaded())
    co_return notLoaded();
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();

  const auto body = ChatCompletionDto::fromJson(*req->getJsonObject());

  auto job = std::make_shared<ChatStreamJob>();
  job->owner = this;
  const auto tools = registeredTools();
  if (tools.empty())
    job->request = body.request();
  else {
    job->loop = toolLoopInput(body, tools, service_.defaultMaxTokens());
    job->history = toChatMessages(body);
  }

  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [job](drogon::ResponseStreamPtr stream) {
        job->stream = std::move(stream);
        std::thread([job] { runStreamJob(job); }).detach();
      },
      true);
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
  co_return resp;
}

drogon::Task<drogon::HttpResponsePtr>
LlmController::engine(drogon::HttpRequestPtr)
{
  const LlmPrefillStats stats = service_.lastPrefillStats();
  Json::Value info(Json::objectValue);
  info["loaded"] = service_.isLoaded();
  info["defaultMaxTokens"] = service_.defaultMaxTokens();
  info["defaultTemperature"] = service_.defaultTemperature();
  info["contextSize"] = Json::Value::Int64(service_.contextSize());
  info["lastPromptTokens"] = stats.promptTokens;
  info["lastReusedTokens"] = stats.reusedTokens;
  info["lastDecodedTokens"] = stats.decodedTokens;
  co_return ApiResponse::ok(info);
}

void LlmController::initEngine()
{
  service_.init();
}

void LlmController::shutdownEngine()
{
  service_.shutdown();
}

bool LlmController::isEngineLoaded()
{
  return service_.isLoaded();
}
