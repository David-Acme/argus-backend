#pragma once

#include <json/value.h>
#include <feature/synthesis/domain/tts-service.hxx>
#include <validation/validation_dsl.hxx>
#include <string>

struct SynthesizeDto
{
  std::string text;
  std::string styleId;
  std::string lang;
  double speed{0};

  static SynthesizeDto fromJson(const Json::Value& json);

  TtsRequest request() const;
};
