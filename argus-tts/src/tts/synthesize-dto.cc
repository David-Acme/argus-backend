#include "synthesize-dto.hxx"

namespace
{
// Mirrors the TtsLang enumerators (EN..NA); used to map the wire language
// code onto the engine enum.
TtsLang ttsLangFromCode(const std::string& code)
{
  for (int i = 0; i < 32; ++i) {
    if (code == langCode(static_cast<TtsLang>(i)))
      return static_cast<TtsLang>(i);
  }
  return TtsLang::EN;
}

} // namespace

SynthesizeDto SynthesizeDto::fromJson(const Json::Value& json)
{
  SynthesizeDto dto;
  dto.text = json.get("text", "").asString();
  dto.styleId = json.get("style_id", "M3").asString();
  dto.lang = json.get("lang", "en").asString();
  dto.speed = json.get("speed", 0.0).asDouble();

  START_VALIDATION(SynthesizeDto, dto)
  IS_NOT_EMPTY(text)
  MAX_LENGTH(text, 4000)
  CUSTOM_LAMBDA(styleId, [](const SynthesizeDto& value)
                -> std::optional<std::string> {
    if (value.styleId.size() > 16)
      return "style_id must be at most 16 characters";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(lang, [](const SynthesizeDto& value)
                -> std::optional<std::string> {
    if (value.lang.empty())
      return std::nullopt;
    for (const auto& code : supportedLangCodes()) {
      if (code == value.lang)
        return std::nullopt;
    }
    return "lang is not a supported code";
  })
  CUSTOM_LAMBDA(speed, [](const SynthesizeDto& value)
                -> std::optional<std::string> {
    if (value.speed == 0)
      return std::nullopt;
    if (value.speed < 0.1 || value.speed > 10)
      return "speed must be between 0.1 and 10";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}

TtsRequest SynthesizeDto::request() const
{
  TtsRequest req;
  req.text = text;
  req.voiceId = styleId;
  req.lang = lang.empty() ? TtsLang::EN : ttsLangFromCode(lang);
  if (speed > 0)
    req.speed = static_cast<float>(speed);
  return req;
}
