#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct PairingDto
{
  std::string code;

  static PairingDto fromJson(const Json::Value& json);
};
