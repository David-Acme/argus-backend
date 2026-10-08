#include "llm-controller.hxx"

#include <auth/module-gate.hxx>
#include <feature/llm/services/tools/app-command.hxx>

#include <errors/response-exception.hxx>
#include <http/api-response.hxx>
#include <llm/llm-errors.hxx>
#include <feature/llm/dtos/chat-dto.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <runtime/blocking-pool.hxx>
#include <runtime/blocking-task.hxx>

#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
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

ToolAudience audienceOf(const ChatRequest& request)
{
  return {.role = request.role, .modules = moduleGate().snapshot()};
}

std::vector<tools::ToolHandle> requestTools(const ToolExecutor& executor, const ChatRequest& request)
{
  if (!request.toolsEnabled)
    return {};
  auto tools = executor.offered(audienceOf(request));
  if (!request.clientActions)
    std::erase_if(tools, [](const tools::ToolHandle& tool) { return isAppTool(tool->spec.name); });
  return tools;
}

std::atomic<int64_t>& turnCounter()
{
  static std::atomic<int64_t> turns{0};
  return turns;
}

struct ToolLoopInputArgs
{
  const std::vector<tools::ToolHandle>& tools;
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
  input.tools = args.tools;
  input.audience = audienceOf(args.request);
  input.contextFacts = args.request.contextFacts;
  input.context = tools::ToolContext{.userId = args.request.userId,
                                     .role = args.request.role,
                                     .lang = LfmAdapter::replyLang(args.request),
                                     .sessionId = args.request.sessionId,
                                     .channel = "tool_result",
                                     .utterance = {},
                                     .decided = false,
                                     .turn = ++turnCounter(),
                                     .emitAction = actionEmitter(args.onAction)};
  input.clock = LfmAdapter::clockNote(args.request.messages, input.context.lang);
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
  StreamLease lease;
  LlmController* owner{nullptr};
  ChatRequest request;
  std::unique_ptr<drogon::ResponseStream> stream;
  LlmPrefillStats stats;
  size_t tokenCount{0};
  size_t charCount{0};
};

bool sameSecret(std::string_view presented, std::string_view expected)
{
  if (presented.size() != expected.size())
    return false;
  unsigned diff = 0U;
  for (std::size_t index = 0; index < presented.size(); ++index)
    diff |= static_cast<unsigned>(static_cast<unsigned char>(presented[index])) ^
            static_cast<unsigned>(static_cast<unsigned char>(expected[index]));
  return diff == 0U;
}

int streamCapacity()
{
  return std::max(1, blocking_pool::limitsFor(BlockingLane::Heavy).maxThreads - 1);
}

std::string withoutSentinelMark(std::string token)
{
  std::erase(token, kStreamSentinelMark);
  return token;
}

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
    if (!job->stream || job->lease.stopping())
      throw ClientGone{};
    if (!done) {
      if (!job->stream->send(withoutSentinelMark(token))) {
        job->stream.reset();
        throw ClientGone{};
      }
      ++job->tokenCount;
      job->charCount += token.size();
      return;
    }
    const std::string line = kStreamSentinelMark + sentinelLine(job->stats);
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
  job->lease.release();
}

}

ChatRequest boundToCaller(ChatRequest request, std::string_view caller)
{
  if (caller == kIdentityCaller)
    return request;
  request.userId = 0;
  request.role = UserRole::Unknown;
  request.toolsEnabled = false;
  request.clientActions = false;
  request.sessionId.clear();
  return request;
}

LlmController::LlmController()
    : adapter_(service_, &intentGate_.router()), streams_(streamCapacity())
{
}

void LlmController::setIdentityCredential(std::string credential)
{
  identityCredential_ = std::move(credential);
}

ChatRequest boundToCredential(ChatRequest request, const CallerCredential& credential)
{
  if (credential.expected.empty())
    return request;
  const bool identified = sameSecret(credential.presented, credential.expected);
  return boundToCaller(std::move(request), identified ? kIdentityCaller : std::string_view{});
}

LlmChatOutcome LlmController::chatSync(const ChatRequest& request)
{
  LlmChatOutcome outcome;
  const auto tools = requestTools(adapter_.executor(), request);
  if (tools.empty()) {
    const ToolChatOutput plain = adapter_.chatPlain(request);
    outcome.text = plain.reply;
    outcome.rawReply = plain.rawReply;
    outcome.contextBlock = plain.contextBlock;
    outcome.generateMs = plain.generateMs;
    return outcome;
  }
  const ToolChatInput loop = toolLoopInput(
      {.tools = tools, .request = request, .defaultMaxTokens = service_.defaultMaxTokens(), .onAction = {}});
  std::vector<ChatMessage> history = request.messages;
  const ToolChatOutput output = adapter_.chatWithTools(loop, history);
  outcome.text = output.reply;
  outcome.rawReply = output.rawReply;
  outcome.contextBlock = output.contextBlock;
  outcome.hops = output.hops;
  outcome.toolCalls = output.executed.size();
  outcome.attempted = output.executed;
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
  const auto tools = requestTools(adapter_.executor(), input.request);
  if (tools.empty()) {
    adapter_.chatPlainStream({.request = input.request, .onToken = emit});
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
      [this, request = boundToCredential(body.request(),
                                         {.presented = req->getHeader(kCallerCredentialHeader),
                                          .expected = identityCredential_})] {
        return chatSync(request);
      },
      BlockingLane::Heavy);
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

  auto lease = streams_.tryAcquire();
  if (!lease)
    throw ResponseException(streams_.stopping() ? LlmErrors::Unavailable : LlmErrors::Busy);
  auto job = std::make_shared<ChatStreamJob>(ChatStreamJob{
      .lease = std::move(*lease),
      .owner = this,
      .request = boundToCredential(body.request(),
                                   {.presented = req->getHeader(kCallerCredentialHeader),
                                    .expected = identityCredential_}),
      .stream = nullptr,
      .stats = {},
      .tokenCount = 0,
      .charCount = 0});

  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [job](drogon::ResponseStreamPtr stream) {
        job->stream = std::move(stream);
        if (!blocking_pool::trySubmit(BlockingLane::Heavy, [job] { runStreamJob(job); })) {
          LOG_WARN << "LLM stream: the heavy lane is full, the stream ends unanswered";
          job->stream->close();
          job->stream.reset();
        }
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
  Json::Value decisions(Json::arrayValue);
  for (const auto& tally : adapter_.flow().decisions()) {
    Json::Value row(Json::objectValue);
    row["decider"] = tally.key.decider;
    row["exact"] = tally.key.exact;
    row["family"] = tally.key.family;
    row["lang"] = tally.key.lang;
    row["verdict"] = tally.key.verdict;
    row["count"] = Json::Value::UInt64(tally.count);
    decisions.append(std::move(row));
  }
  info["decisions"] = std::move(decisions);
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
