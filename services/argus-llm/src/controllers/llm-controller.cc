#include "llm-controller.hxx"

#include <llm/chat-dto.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <drogon/drogon.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

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

// Producer state for the chunked stream leg.
struct ChatStreamJob
{
  LlmController* owner{nullptr};
  ChatRequest request;
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
  try {
    service.chatStream(job->request,
                       [&job, &service](const std::string& token, bool done) {
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
                         const std::string line =
                             "\n" + sentinelLine(service.lastPrefillStats());
                         if (!job->stream->send(line))
                           job->stream.reset();
                       });
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
  std::string text = co_await service_.chatAsync(body.request());
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "LLM chat: messages=" << body.messages.size()
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
  job->request = body.request();
  job->owner = this;

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
