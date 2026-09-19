#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct PairingDto
{
  std::string code;

  static PairingDto fromJson(const Json::Value& json);
};
