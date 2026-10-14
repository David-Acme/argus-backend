#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct RegisterNotificationTokenDto
{
  std::string token;
  std::string platform;
  std::string lang;
  std::string provider;
  std::string publicKey;
  std::string keyId;

  static RegisterNotificationTokenDto fromJson(const Json::Value& json);
};
