#include "stt-controller.hxx"

#include "stt-errors.hxx"

#include <errors/response-exception.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <http/api-response.hxx>
#include <stt/stt-remote.hxx>

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr const char* kPcmMime = "audio/x-argus-pcm-s16";

std::vector<float> pcmFromBytes(std::string_view bytes)
{
  std::vector<float> samples(bytes.size() / sizeof(int16_t));
  for (size_t i = 0; i < samples.size(); ++i) {
    int16_t raw = 0;
    std::memcpy(&raw, bytes.data() + i * sizeof(int16_t), sizeof(raw));
    samples[i] = static_cast<float>(raw) / kPcmScale;
  }
  return samples;
}

std::string langOf(const drogon::HttpRequestPtr& req)
{
  std::string lang = req->getParameter("lang");
  if (lang.empty())
    lang = req->getHeader("lang");
  return lang;
}

}

drogon::Task<drogon::HttpResponsePtr>
SttController::transcribe(drogon::HttpRequestPtr req)
{
  auto& stt = SttService::instance();
  if (!stt.isLoaded())
    throw ResponseException(SttErrors::SpeechEngineNotLoaded);

  const std::string_view body = req->getBody();
  const std::string contentType =
      std::string(req->getHeader("content-type"));
  if (contentType.find(kPcmMime) == std::string::npos)
    throw ResponseException(SttErrors::BodyNotPcmS16);
  if (body.size() % sizeof(int16_t) != 0)
    throw ResponseException(SttErrors::PcmBodyMisaligned);
  if (body.empty()) {
    Json::Value fields(Json::objectValue);
    fields["body"] = "empty pcm body";
    co_return ApiResponse::validationError(fields);
  }

  const std::string lang = langOf(req);
  const std::string effective = lang.empty() ? stt.configLanguage() : lang;
  if (!stt.isSupportedLanguage(effective)) {
    Json::Value fields(Json::objectValue);
    fields["lang"] = "unsupported language (es, en, auto)";
    co_return ApiResponse::validationError(fields);
  }

  const std::vector<float> samples = pcmFromBytes(body);
  const auto t0 = std::chrono::steady_clock::now();
  std::string text = co_await stt.transcribeAsync(
      {.audioSamples = samples, .sampleRate = kWireSampleRate, .lang = lang});
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  LOG_INFO << "STT transcribe: samples=" << samples.size()
           << " lang=" << stt.language() << " ms=" << static_cast<int>(ms);

  Json::Value info(Json::objectValue);
  info["text"] = std::move(text);
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
