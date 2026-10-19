#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

struct CreateProximityReceiverDto
{
  std::string name;
  std::string room;
  std::string kind;
  std::string host;
  int64_t port{80};

  static CreateProximityReceiverDto fromJson(const Json::Value& json);
};
