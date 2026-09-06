#include "stt-controller.hxx"

#include <shared/services/stt/remote/stt-remote.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr const char* kPcmMime = "audio/x-argus-pcm-s16";

// The voice session feeds float samples; its own frame conversion is
// int16/kPcmScale (voice-session-service.cc), so the inverse maps back
// losslessly up to quantization.
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

// lang comes on the query string (?lang=es) or the `lang` header; "" keeps
// the service-configured default.
std::string langOf(const drogon::HttpRequestPtr& req)
{
  std::string lang = req->getParameter("lang");
  if (lang.empty())
    lang = req->getHeader("lang");
  return lang;
}

} // namespace

drogon::Task<drogon::HttpResponsePtr>
SttController::transcribe(drogon::HttpRequestPtr req)
{
  auto& stt = SttService::instance();
  if (!stt.isLoaded())
    co_return ApiResponse::error(503, "STT_NOT_LOADED",
                                 "Speech-to-text engine is not loaded");

  const std::string_view body = req->getBody();
  const std::string contentType =
      std::string(req->getHeader("content-type"));
  if (contentType.find(kPcmMime) == std::string::npos)
    co_return ApiResponse::error(400, "BAD_REQUEST",
                                 "Body must be audio/x-argus-pcm-s16");
  if (body.size() % sizeof(int16_t) != 0)
    co_return ApiResponse::error(400, "BAD_REQUEST",
                                 "PCM body is not int16-aligned");
  if (body.empty()) {
    Json::Value fields(Json::objectValue);
    fields["body"] = "empty pcm body";
    co_return ApiResponse::validationError(fields);
  }

  // Ruling BE: one global recognizer; a lang different from the current one
  // rebuilds it inside the transcribe's blocking leg (never on this IO
  // thread). Unsupported langs are a 422, mirroring SttService::setLanguage's
  // accepted set; empty resolves from stt.language.
  const std::string lang = langOf(req);
  const std::string effective = lang.empty() ? stt.configLanguage() : lang;
  if (!stt.isSupportedLanguage(effective)) {
    Json::Value fields(Json::objectValue);
    fields["lang"] = "unsupported language (es, en, auto)";
    co_return ApiResponse::validationError(fields);
  }

  const std::vector<float> samples = pcmFromBytes(body);
  const auto t0 = std::chrono::steady_clock::now();
  std::string text = co_await stt.transcribeAsync(samples, kWireSampleRate, lang);
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
