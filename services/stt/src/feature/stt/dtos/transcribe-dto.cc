#include "transcribe-dto.hxx"

#include <errors/response-exception.hxx>
#include <stt/stt-errors.hxx>
#include <stt/stt-remote.hxx>
#include <validation/validation_dsl.hxx>

#include <cstdint>
#include <cstring>
#include <optional>

namespace
{
constexpr std::string_view kPcmMime = "audio/x-argus-pcm-s16";
constexpr std::size_t kMaxPcmBytes =
    kMaxTranscribeSeconds * static_cast<std::size_t>(kWireSampleRate) * sizeof(int16_t);

std::string langOf(const drogon::HttpRequestPtr& req)
{
  std::string lang = req->getParameter("lang");
  if (lang.empty())
    lang = req->getHeader("lang");
  return lang;
}
}

TranscribeDto TranscribeDto::fromRequest(const drogon::HttpRequestPtr& req)
{
  if (req->getHeader("content-type").find(kPcmMime) == std::string::npos)
    throw ResponseException(SttErrors::BodyNotPcmS16);
  TranscribeDto dto;
  dto.body = req->getBody();
  dto.lang = langOf(req);
  if (dto.body.size() % sizeof(int16_t) != 0)
    throw ResponseException(SttErrors::PcmBodyMisaligned);

  START_VALIDATION(TranscribeDto, dto)
  CUSTOM_LAMBDA(body, [](const TranscribeDto& value) -> std::optional<std::string> {
    if (value.body.empty())
      return "empty pcm body";
    if (value.body.size() > kMaxPcmBytes)
      return "pcm body longer than 120 s";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}

std::vector<float> TranscribeDto::samples() const
{
  std::vector<float> out(body.size() / sizeof(int16_t));
  for (std::size_t i = 0; i < out.size(); ++i) {
    int16_t raw = 0;
    std::memcpy(&raw, body.data() + i * sizeof(int16_t), sizeof(raw));
    out[i] = static_cast<float>(raw) / kPcmScale;
  }
  return out;
}
