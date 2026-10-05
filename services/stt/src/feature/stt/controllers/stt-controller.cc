#include "stt-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/stt/dtos/transcribe-dto.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <runtime/blocking-task.hxx>
#include <http/api-response.hxx>
#include <stt/stt-errors.hxx>
#include <stt/stt-remote.hxx>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace
{

struct TranscribedPcm
{
  std::string text;
  std::size_t samples{0};
};

int elapsedMs(std::chrono::steady_clock::time_point start)
{
  return static_cast<int>(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count());
}

}

drogon::Task<drogon::HttpResponsePtr>
SttController::transcribe(drogon::HttpRequestPtr req)
{
  auto& stt = SttService::instance();
  if (!stt.isLoaded())
    throw ResponseException(SttErrors::SpeechEngineNotLoaded);
  const TranscribeDto body = TranscribeDto::fromRequest(req);
  if (!stt.isSupportedLanguage(body.lang.empty() ? stt.configLanguage() : body.lang)) {
    Json::Value fields(Json::objectValue);
    fields["lang"] = "unsupported language (es, en, auto)";
    co_return ApiResponse::validationError(fields);
  }

  const auto t0 = std::chrono::steady_clock::now();
  const TranscribedPcm heard = co_await BlockingTask<TranscribedPcm>(
      [&stt, req, lang = body.lang] {
        const TranscribeDto pcm{.body = req->getBody(), .lang = lang};
        TranscribeRequest request{.samples = pcm.samples(), .sampleRate = kWireSampleRate, .lang = lang};
        const std::size_t samples = request.samples.size();
        return TranscribedPcm{.text = stt.transcribe(request), .samples = samples};
      },
      BlockingLane::Heavy);
  LOG_INFO << "STT transcribe: samples=" << heard.samples << " lang=" << stt.language()
           << " ms=" << elapsedMs(t0);

  Json::Value info(Json::objectValue);
  info["text"] = heard.text;
  co_return ApiResponse::ok(info);
}

drogon::Task<drogon::HttpResponsePtr>
SttController::engine(drogon::HttpRequestPtr)
{
  auto& stt = SttService::instance();
  Json::Value info(Json::objectValue);
  info["language"] = stt.language();
  info["defaultLanguage"] = stt.configLanguage();
  info["loaded"] = stt.isLoaded();
  co_return ApiResponse::ok(info);
}
