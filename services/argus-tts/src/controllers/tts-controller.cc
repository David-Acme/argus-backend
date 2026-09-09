#include "tts-controller.hxx"

#include <tts/synthesize-dto.hxx>

#include <shared/services/tts/tts-service.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace
{
constexpr const char* kPcmMime = "audio/x-argus-pcm-f32";

drogon::HttpResponsePtr notLoaded()
{
  return ApiResponse::error(503, "TTS_NOT_LOADED",
                            "Text-to-speech engine is not loaded");
}

std::string pcmBytes(const std::vector<float>& pcm)
{
  std::string bytes(pcm.size() * sizeof(float), '\0');
  if (!pcm.empty())
    std::memcpy(bytes.data(), pcm.data(), bytes.size());
  return bytes;
}

// Producer state for the chunked stream leg.
struct PcmStreamJob
{
  TtsRequest request;
  std::unique_ptr<drogon::ResponseStream> stream;
  size_t chunkCount{0};
  size_t byteCount{0};
};

void runStreamJob(const std::shared_ptr<PcmStreamJob>& job)
{
  const auto t0 = std::chrono::steady_clock::now();
  try {
    TtsService::instance().synthesizeStream(
        job->request, [&job](const std::vector<float>& chunk) {
          if (!job->stream)
            return;
          const auto bytes = pcmBytes(chunk);
          if (!job->stream->send(bytes)) {
            job->stream.reset();
            return;
          }
          job->chunkCount++;
          job->byteCount += bytes.size();
        });
  }
  catch (const std::exception& e) {
    LOG_ERROR << "TTS stream synthesis failed: " << e.what();
  }
  if (job->stream) {
    job->stream->close();
    job->stream.reset();
  }
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "TTS stream: chunks=" << job->chunkCount << " bytes="
           << job->byteCount << " ms=" << static_cast<int>(ms);
}

} // namespace

drogon::Task<drogon::HttpResponsePtr>
TtsController::synthesize(drogon::HttpRequestPtr req)
{
  const auto body = SynthesizeDto::fromJson(*req->getJsonObject());
  auto& tts = TtsService::instance();
  if (!tts.isLoaded())
    co_return notLoaded();

  const auto t0 = std::chrono::steady_clock::now();
  auto pcm = co_await tts.synthesizeAsync(body.request());
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "TTS synthesize: samples=" << pcm.size()
           << " ms=" << static_cast<int>(ms);

  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_CUSTOM);
  resp->setContentTypeString(kPcmMime);
  resp->addHeader("X-Argus-Sample-Rate",
                  std::to_string(tts.sampleRate()));
  resp->setBody(pcmBytes(pcm));
  co_return resp;
}

drogon::Task<drogon::HttpResponsePtr>
TtsController::synthesizeStream(drogon::HttpRequestPtr req)
{
  const auto body = SynthesizeDto::fromJson(*req->getJsonObject());
  auto& tts = TtsService::instance();
  if (!tts.isLoaded())
    co_return notLoaded();

  auto job = std::make_shared<PcmStreamJob>();
  job->request = body.request();

  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [job](drogon::ResponseStreamPtr stream) {
        job->stream = std::move(stream);
        std::thread([job] { runStreamJob(job); }).detach();
      },
      true);
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_CUSTOM);
  resp->setContentTypeString(kPcmMime);
  resp->addHeader("X-Argus-Sample-Rate", std::to_string(tts.sampleRate()));
  co_return resp;
}

drogon::Task<drogon::HttpResponsePtr>
TtsController::engine(drogon::HttpRequestPtr)
{
  auto& tts = TtsService::instance();
  Json::Value info(Json::objectValue);
  info["sampleRate"] = tts.sampleRate();
  info["defaultSpeed"] = tts.defaultSpeed();
  info["loaded"] = tts.isLoaded();
  co_return ApiResponse::ok(info);
}
