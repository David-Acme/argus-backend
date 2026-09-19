#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

// /camera/{id}/talk body: lang is `es` or `en` and picks the TTS voice.
struct CameraTalkDto
{
  std::string text;
  std::string lang;

  static CameraTalkDto fromJson(const Json::Value& json);
};
