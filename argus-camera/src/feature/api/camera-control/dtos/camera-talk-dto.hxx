#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CameraTalkDto
{
  std::string text;
  /** `es` or `en`; picks the TTS voice. */
  std::string lang;

  static CameraTalkDto fromJson(const Json::Value& json);
};
