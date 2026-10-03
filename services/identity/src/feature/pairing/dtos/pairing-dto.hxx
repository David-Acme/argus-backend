#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct PairingDto
{
  std::string code;
  std::string nonce;
  std::string proof;

  static PairingDto fromJson(const Json::Value& json);
};
