#include "llm-controller.hxx"

#include <feature/llm/services/tools/app-tool-descriptors.hxx>

#include <errors/response-exception.hxx>
#include <http/api-response.hxx>
#include <llm/llm-errors.hxx>
#include <feature/llm/dtos/chat-dto.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <runtime/blocking-task.hxx>

#include <drogon/drogon.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

drogon::HttpResponsePtr notLoaded()
{
  throw ResponseException(LlmErrors::LlmEngineNotLoaded);
}

drogon::HttpResponsePtr badRequest()
{
  throw ResponseException(LlmErrors::BodyNotJsonObject);
}

constexpr std::string_view kDefaultToolLang = "es";

constexpr const char* kToolPolicy =
    "Eres Argus. Si el usuario pide guardar o recordar algo, usa "
    "memory.remember. Si no, responde brevemente.";

constexpr const char* kClientActionPolicy =
    " Estás en una llamada y la app del usuario está abierta: si pide ver una "
    "cámara usa app.show_camera, si pide abrir una sección usa app.open, y si "
    "pide cambiar la vigilancia o dice que se va, que duerme o que vuelve, usa "
    "app.set_guard_mode. Confirma en una frase lo que hiciste.";

std::vector<const tools::ToolDescriptor*> requestTools(const ChatRequest& request)
{
  if (!request.toolsEnabled)
    return {};
  auto tools = ToolExecutor(ToolRegistry::instance()).permittedTools(request.role);
  if (!request.clientActions)
    std::erase_if(tools, [](const tools::ToolDescriptor* tool) { return isAppTool(tool->name); });
  return tools;
}

struct ToolLoopInputArgs
{
  const std::vector<const tools::ToolDescriptor*>& tools;
  ChatRequest request;
  int32_t defaultMaxTokens{0};
  ActionCallback onAction{};
};

std::function<void(const std::string&, const Json::Value&)> actionEmitter(ActionCallback onAction)
{
  if (!onAction)
    return {};
  return [onAction = std::move(onAction)](const std::string& name, const Json::Value& arguments) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    onAction({.name = name, .arguments = Json::writeString(builder, arguments)});
  };
}

ToolChatInput toolLoopInput(const ToolLoopInputArgs& args)
{
  ToolChatInput input;
  input.systemPrompt = args.request.clientActions
                           ? std::string(kToolPolicy) + kClientActionPolicy
                           : std::string(kToolPolicy);
  input.tools = args.tools;
  input.role = args.request.role;
  input.context = tools::ToolContext{.userId = args.request.userId,
                                     .lang = args.request.lang.empty()
                                                 ? std::string(kDefaultToolLang)
                                                 : args.request.lang,
                                     .sessionId = args.request.sessionId,
                                     .channel = "tool_result",
                                     .utterance = {},
                                     .decided = false,
                                     .emitAction = actionEmitter(args.onAction)};
  input.maxHops = 3;
  input.temperature = args.request.temperature;
  input.resetContext = args.request.resetContext;
  input.answerMaxTokens = args.request.maxTokens > 0 ? args.request.maxTokens
                                                     : args.defaultMaxTokens;
  input.prefillOnly = args.request.prefillOnly;
  return input;
}

struct ClientGone
{
};

struct ChatStreamJob
{
  LlmController* owner{nullptr};
  ChatRequest request;
  std::unique_ptr<drogon::ResponseStream> stream;
  LlmPrefillStats stats;
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
  const TokenCallback send = [&job](const std::string& token, bool done) {
    if (!job->stream)
      throw ClientGone{};
    if (!done) {
      if (!job->stream->send(token)) {
        job->stream.reset();
        throw ClientGone{};
      }
      ++job->tokenCount;
      job->charCount += token.size();
      return;
    }
    const std::string line = "\n" + sentinelLine(job->stats);
    if (!job->stream->send(line))
      job->stream.reset();
  };
  try {
    job->owner->chatStreamSync(
        {.request = job->request,
         .onToken = send,
         .stats = &job->stats,
         .cancellation = {}});
  }
  catch (const ClientGone&) {
    LOG_INFO << "LLM stream: client left, generation stopped";
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

}

LlmChatOutcome LlmController::chatSync(const ChatRequest& request)
{
  LlmChatOutcome outcome;
  const auto tools = requestTools(request);
  if (tools.empty()) {
    outcome.text = service_.chat(request);
    return outcome;
  }
  const ToolChatInput loop = toolLoopInput(
      {.tools = tools, .request = request, .defaultMaxTokens = service_.defaultMaxTokens(), .onAction = {}});
  std::vector<ChatMessage> history = request.messages;
  const ToolChatOutput output = adapter_.chatWithTools(loop, history);
  outcome.text = output.reply;
  outcome.hops = output.hops;
  outcome.toolCalls = output.executed.size();
  outcome.generateMs = output.generateMs;
  outcome.toolMs = output.toolMs;
  return outcome;
}

void LlmController::chatStreamSync(const LlmStreamInput& input)
{
  LlmPrefillStats* stats = input.stats;
  const TokenCallback emit = [this, stats,
                              forward = input.onToken](const std::string& token,
                                                       bool done) {
    if (done && stats)
      *stats = service_.lastPrefillStats();
    forward(token, done);
  };
  const auto tools = requestTools(input.request);
  if (tools.empty()) {
    service_.chatStream(input.request, emit);
    return;
  }
  const ToolChatInput loop = toolLoopInput({.tools = tools,
                                            .request = input.request,
                                            .defaultMaxTokens =
                                                service_.defaultMaxTokens(),
                                            .onAction = input.onAction});
  std::vector<ChatMessage> history = input.request.messages;
  const ToolChatOutput output = adapter_.chatWithToolsStream(
      {.input = loop, .history = history, .onToken = emit});
  LOG_INFO << "LLM stream loop: hops=" << output.hops
           << " tools=" << output.executed.size()
           << " gen_ms=" << output.generateMs << " tool_ms=" << output.toolMs;
}

drogon::Task<drogon::HttpResponsePtr>
LlmController::chat(drogon::HttpRequestPtr req)
{
  if (!service_.isLoaded())
    co_return notLoaded();
  if (!req->getJsonError().empty() || !req->getJsonObject())
    co_return badRequest();

  const auto body = ChatCompletionDto::fromJson(*req->getJsonObject());

  const auto t0 = std::chrono::steady_clock::now();
  auto outcome = co_await BlockingTask<LlmChatOutcome>(
      [this, request = body.request()] { return chatSync(request); });
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  if (outcome.hops == 0)
    LOG_INFO << "LLM chat: messages=" << body.messages.size()
             << " chars=" << outcome.text.size()
             << " ms=" << static_cast<int>(ms);
  else
    LOG_INFO << "LLM chat loop: hops=" << outcome.hops
             << " tools=" << outcome.toolCalls
             << " gen_ms=" << outcome.generateMs
             << " tool_ms=" << outcome.toolMs
             << " messages=" << body.messages.size()
             << " chars=" << outcome.text.size()
             << " ms=" << static_cast<int>(ms);

  Json::Value info(Json::objectValue);
  info["text"] = outcome.text;
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
  job->request = body.request();

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
