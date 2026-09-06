#pragma once

#include <json/value.h>
#include <shared/services/tts/tts-service.hxx>
#include <shared/validation/validation_dsl.hxx>
#include <string>

// Internal wire request (Ruling BH): {text, style_id?, speed?, lang?}.
struct SynthesizeDto
{
  std::string text;
  std::string styleId;
  std::string lang;
  double speed{0};

  static SynthesizeDto fromJson(const Json::Value& json);

  TtsRequest request() const;
};
